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
 * @file  llpcBuilderImplDesc.cpp
 * @brief LLPC source file: implementation of Builder methods for descriptor loads
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcInternal.h"
#include "llpcTargetInfo.h"
#include "llpcPipelineState.h"

#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-builder-impl-desc"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a load of a buffer descriptor.
Value* BuilderImplDesc::CreateLoadBufferDesc(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    Value*        descIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    bool          isWritten,        // Whether the buffer is (or might be) written to
    Type* const   pointeeTy,       // [in] Type that the returned pointer should point to.
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    assert(pointeeTy );

    Instruction* const insertPos = &*GetInsertPoint();
    descIndex = scalarizeIfUniform(descIndex, isNonUniform);

    // Mark the shader as reading and writing (if applicable) a resource.
    auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
    resUsage->resourceRead = true;
    resUsage->resourceWrite |= isWritten;

    // TODO: This currently creates a call to the llpc.descriptor.* function. A future commit will change it to
    // look up the descSet/binding and generate the code directly.
    auto bufDescLoadCall = emitCall(lgcName::DescriptorLoadBuffer,
                                     VectorType::get(getInt32Ty(), 4),
                                     {
                                         getInt32(descSet),
                                         getInt32(binding),
                                         descIndex,
                                     },
                                     {},
                                     insertPos);
    bufDescLoadCall->setName(instName);

    bufDescLoadCall = emitCall(lgcName::LateLaunderFatPointer,
                                getInt8Ty()->getPointerTo(ADDR_SPACE_BUFFER_FAT_POINTER),
                                bufDescLoadCall,
                                Attribute::ReadNone,
                                insertPos);

    return CreateBitCast(bufDescLoadCall, getBufferDescTy(pointeeTy));
}

// =====================================================================================================================
// Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
Value* BuilderImplDesc::CreateIndexDescPtr(
    Value*        descPtr,           // [in] Descriptor pointer, as returned by this function or one of
                                      //    the CreateGet*DescPtr methods
    Value*        index,             // [in] Index value
    bool          isNonUniform,       // Whether the descriptor index is non-uniform
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    if (index != getInt32(0))
    {
        index = scalarizeIfUniform(index, isNonUniform);
        std::string name = lgcName::DescriptorIndex;
        addTypeMangling(descPtr->getType(), {}, name);
        descPtr = emitCall(name,
                            descPtr->getType(),
                            {
                                descPtr,
                                index,
                            },
                            {},
                            &*GetInsertPoint());
        descPtr->setName(instName);
    }
    return descPtr;
}

// =====================================================================================================================
// Load image/sampler/texelbuffer/F-mask descriptor from pointer.
// Returns <8 x i32> descriptor for image or F-mask, or <4 x i32> descriptor for sampler or texel buffer.
Value* BuilderImplDesc::CreateLoadDescFromPtr(
    Value*        descPtr,           // [in] Descriptor pointer, as returned by CreateIndexDescPtr or one of
                                      //    the CreateGet*DescPtr methods
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
    getPipelineState()->getShaderResourceUsage(m_shaderStage)->useImages = true;

    // Use llpc.descriptor.load.from.ptr.
    std::string name = lgcName::DescriptorLoadFromPtr;
    addTypeMangling(descPtr->getType(), {}, name);
    auto desc = createNamedCall(name,
                                 cast<StructType>(descPtr->getType())->getElementType(0)->getPointerElementType(),
                                 descPtr,
                                 {});
    desc->setName(instName);
    return desc;
}

// =====================================================================================================================
// Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
Value* BuilderImplDesc::CreateGetSamplerDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    // This currently creates calls to the llpc.descriptor.* functions. A future commit will change it to
    // look up the descSet/binding and generate the code directly.
    auto descPtr = emitCall(lgcName::DescriptorGetSamplerPtr,
                             getSamplerDescPtrTy(),
                             {
                                 getInt32(descSet),
                                 getInt32(binding),
                             },
                             {},
                             &*GetInsertPoint());
    descPtr->setName(instName);
    return descPtr;
}

// =====================================================================================================================
// Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
Value* BuilderImplDesc::CreateGetImageDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    // This currently creates calls to the llpc.descriptor.* functions. A future commit will change it to
    // look up the descSet/binding and generate the code directly.
    auto descPtr = emitCall(lgcName::DescriptorGetResourcePtr,
                             getImageDescPtrTy(),
                             {
                                 getInt32(descSet),
                                 getInt32(binding),
                             },
                             {},
                             &*GetInsertPoint());
    descPtr->setName(instName);
    return descPtr;
}

// =====================================================================================================================
// Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
Value* BuilderImplDesc::CreateGetTexelBufferDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    // This currently creates calls to the llpc.descriptor.* functions. A future commit will change it to
    // look up the descSet/binding and generate the code directly.
    auto descPtr = emitCall(lgcName::DescriptorGetTexelBufferPtr,
                             getTexelBufferDescPtrTy(),
                             {
                                 getInt32(descSet),
                                 getInt32(binding),
                             },
                             {},
                             &*GetInsertPoint());
    descPtr->setName(instName);
    return descPtr;
}

// =====================================================================================================================
// Create a pointer to F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
Value* BuilderImplDesc::CreateGetFmaskDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    // This currently creates calls to the llpc.descriptor.* functions. A future commit will change it to
    // look up the descSet/binding and generate the code directly.
    auto descPtr = emitCall(lgcName::DescriptorGetFmaskPtr,
                             getFmaskDescPtrTy(),
                             {
                                 getInt32(descSet),
                                 getInt32(binding),
                             },
                             {},
                             &*GetInsertPoint());
    descPtr->setName(instName);
    return descPtr;
}

// =====================================================================================================================
// Create a load of the push constants table pointer.
// This returns a pointer to the ResourceNodeType::PushConst resource in the top-level user data table.
// The type passed must have the correct size for the push constants.
Value* BuilderImplDesc::CreateLoadPushConstantsPtr(
    Type*         pushConstantsTy, // [in] Type of the push constants table that the returned pointer will point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    // Remember the size of push constants.
    unsigned pushConstSize = GetInsertPoint()->getModule()->getDataLayout().getTypeStoreSize(pushConstantsTy);
    ResourceUsage* resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
    assert((resUsage->pushConstSizeInBytes == 0) || (resUsage->pushConstSizeInBytes == pushConstSize));
    resUsage->pushConstSizeInBytes = pushConstSize;

    auto pushConstantsPtrTy = PointerType::get(pushConstantsTy, ADDR_SPACE_CONST);
    // TODO: This currently creates a call to the llpc.descriptor.* function. A future commit will change it to
    // generate the code directly.
    std::string callName = lgcName::DescriptorLoadSpillTable;
    addTypeMangling(pushConstantsPtrTy, {}, callName);
    auto pushConstantsLoadCall = createNamedCall(callName, pushConstantsPtrTy, {}, {});
    pushConstantsLoadCall->setName(instName);
    return pushConstantsLoadCall;
}

// =====================================================================================================================
// Scalarize a value (pass it through readfirstlane) if uniform
Value* BuilderImplDesc::scalarizeIfUniform(
    Value*  value,       // [in] 32-bit integer value to scalarize
    bool    isNonUniform) // Whether value is marked as non-uniform
{
    assert(value->getType()->isIntegerTy(32));
    if ((!isNonUniform) && (!isa<Constant>(value)))
    {
        // NOTE: GFX6 encounters GPU hang with this optimization enabled. So we should skip it.
        if (getPipelineState()->getTargetInfo().getGfxIpVersion().major > 6)
            value = CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, value);
    }
    return value;
}

// =====================================================================================================================
// Create a buffer length query based on the specified descriptor.
Value* BuilderImplDesc::CreateGetBufferDescLength(
    Value* const  bufferDesc,      // [in] The buffer descriptor to query.
    const Twine&  instName)         // [in] Name to give instruction(s).
{
    // In future this should become a full LLVM intrinsic, but for now we patch in a late intrinsic that is cleaned up
    // in patch buffer op.
    Instruction* const insertPos = &*GetInsertPoint();
    std::string callName = lgcName::LateBufferLength;
    addTypeMangling(nullptr, bufferDesc, callName);
    return emitCall(callName,
                    getInt32Ty(),
                    bufferDesc,
                    Attribute::ReadNone,
                    insertPos);
}

