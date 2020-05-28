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
 * @file  DescBuilder.cpp
 * @brief LLPC source file: implementation of Builder methods for descriptor loads
 ***********************************************************************************************************************
 */
#include "BuilderImpl.h"
#include "lgc/LgcContext.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-builder-impl-desc"

using namespace lgc;
using namespace llvm;

namespace {
// Descriptor sizes that are not part of hardware. Hardware-defined ones are in TargetInfo.
const unsigned DescriptorSizeBufferCompact = 2 * sizeof(unsigned);
const unsigned DescriptorSizeSamplerYCbCr = 8 * sizeof(unsigned);
} // anonymous namespace

// =====================================================================================================================
// Create a load of a buffer descriptor.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param descIndex : Descriptor index
// @param isNonUniform : Whether the descriptor index is non-uniform
// @param isWritten : Whether the buffer is (or might be) written to
// @param pointeeTy : Type that the returned pointer should point to.
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateLoadBufferDesc(unsigned descSet, unsigned binding, Value *descIndex, bool isNonUniform,
                                         bool isWritten, Type *const pointeeTy, const Twine &instName) {
  Value *desc = nullptr;
  descIndex = scalarizeIfUniform(descIndex, isNonUniform);

  // Mark the shader as reading and writing (if applicable) a resource.
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
  resUsage->resourceRead = true;
  resUsage->resourceWrite |= isWritten;

  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    // We have the user data layout. Find the node.
    std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorBuffer, descSet, binding);
    if (!node) {
      // We did not find the resource node. Return an undef value.
      return UndefValue::get(getBufferDescTy(pointeeTy));
    }

    if (node == topNode && isa<Constant>(descIndex)) {
      // Handle a descriptor in the root table (a "dynamic descriptor") specially, as long as it is not variably
      // indexed. This lgc.root.descriptor call is by default lowered in PatchEntryPointMutate into a load from the
      // spill table, but it might be able to "unspill" it to directly use shader entry SGPRs.
      unsigned byteSize = getLgcContext()->getTargetInfo().getGpuProperty().descriptorSizeBuffer;
      if (node->type == ResourceNodeType::DescriptorBufferCompact)
        byteSize = DescriptorSizeBufferCompact;
      unsigned dwordSize = byteSize / 4;
      Type *descTy = VectorType::get(getInt32Ty(), dwordSize);
      std::string callName = lgcName::RootDescriptor;
      addTypeMangling(descTy, {}, callName);
      unsigned dwordOffset = cast<ConstantInt>(descIndex)->getZExtValue() * dwordSize;
      if (dwordOffset + dwordSize > node->sizeInDwords) {
        // Index out of range
        desc = UndefValue::get(descTy);
      } else {
        dwordOffset += node->offsetInDwords;
        desc = CreateNamedCall(callName, descTy, getInt32(dwordOffset), Attribute::ReadNone);
      }
    } else if (node->type == ResourceNodeType::InlineBuffer) {
      // Handle an inline buffer specially. Get a pointer to it, then expand to a descriptor.
      Value *descPtr = getDescPtr(node->type, descSet, binding, topNode, node, /*shadow=*/false);
      desc = buildInlineBufferDesc(descPtr);
    }
  }

  if (!desc) {
    // Not handled by either of the special cases above...
    // Get a pointer to the descriptor, as a pointer to i8, in a struct with the stride.
    Value *descPtrAndStride =
        getDescPtrAndStride(node ? node->type : ResourceNodeType::DescriptorBuffer, descSet, binding, topNode, node,
                            /*shadow=*/false);

    // Index it.
    if (descIndex != getInt32(0))
      descPtrAndStride = CreateIndexDescPtr(descPtrAndStride, descIndex, isNonUniform, "");
    Value *descPtr = CreateExtractValue(descPtrAndStride, 0);

    // Load it.
    desc = CreateLoad(descPtr->getType()->getPointerElementType(), descPtr);
  }

  // If it is a compact buffer descriptor, expand it. (That can only happen when user data layout is available;
  // compact buffer descriptors are disallowed when using shader compilation with no user data layout).
  if (node && node->type == ResourceNodeType::DescriptorBufferCompact)
    desc = buildBufferCompactDesc(desc);

  if (!instName.isTriviallyEmpty())
    desc->setName(instName);

  // Convert to fat pointer.
  desc = CreateNamedCall(lgcName::LateLaunderFatPointer, getInt8Ty()->getPointerTo(ADDR_SPACE_BUFFER_FAT_POINTER), desc,
                         Attribute::ReadNone);
  return CreateBitCast(desc, getBufferDescTy(pointeeTy));
}

// =====================================================================================================================
// Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
//
// @param descPtrStruct : Descriptor pointer struct, as returned by this function or one of the
//                        CreateGet*DescPtr methods
// @param index : Index value
// @param isNonUniform : Whether the descriptor index is non-uniform
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateIndexDescPtr(Value *descPtrStruct, Value *index, bool isNonUniform, const Twine &instName) {
  if (index == getInt32(0))
    return descPtrStruct;

  index = scalarizeIfUniform(index, isNonUniform);
  Value *stride = CreateExtractValue(descPtrStruct, 1);
  Value *descPtr = CreateExtractValue(descPtrStruct, 0);

  Value *bytePtr = CreateBitCast(descPtr, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));
  index = CreateMul(index, stride);
  bytePtr = CreateGEP(getInt8Ty(), bytePtr, index, instName);
  descPtr = CreateBitCast(bytePtr, descPtr->getType());

  descPtrStruct =
      CreateInsertValue(UndefValue::get(StructType::get(getContext(), {descPtr->getType(), getInt32Ty()})), descPtr, 0);
  descPtrStruct = CreateInsertValue(descPtrStruct, stride, 1);

  return descPtrStruct;
}

// =====================================================================================================================
// Load image/sampler/texelbuffer/F-mask descriptor from pointer.
// Returns <8 x i32> descriptor for image or F-mask, or <4 x i32> descriptor for sampler or texel buffer.
//
// @param descPtrStruct : Descriptor pointer struct, as returned by CreateIndexDescPtr or one of the
//                        CreateGet*DescPtr methods
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateLoadDescFromPtr(Value *descPtrStruct, const Twine &instName) {
  // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
  getPipelineState()->getShaderResourceUsage(m_shaderStage)->useImages = true;

  Value *descPtr = CreateExtractValue(descPtrStruct, 0);
  return CreateLoad(descPtr->getType()->getPointerElementType(), descPtr, instName);
}

// =====================================================================================================================
// Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateGetSamplerDescPtr(unsigned descSet, unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorSampler, descSet, binding);
    if (!node) {
      // We did not find the resource node. Return an undef value.
      return UndefValue::get(getSamplerDescPtrTy());
    }
  }

  // Get the descriptor pointer and stride as a struct.
  return getDescPtrAndStride(ResourceNodeType::DescriptorSampler, descSet, binding, topNode, node, /*shadow=*/false);
}

// =====================================================================================================================
// Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateGetImageDescPtr(unsigned descSet, unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorResource, descSet, binding);
    if (!node) {
      // We did not find the resource node. Return an undef value.
      return UndefValue::get(getImageDescPtrTy());
    }
  }

  // Get the descriptor pointer and stride as a struct.
  return getDescPtrAndStride(ResourceNodeType::DescriptorResource, descSet, binding, topNode, node, /*shadow=*/false);
}

// =====================================================================================================================
// Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateGetTexelBufferDescPtr(unsigned descSet, unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) =
        m_pipelineState->findResourceNode(ResourceNodeType::DescriptorTexelBuffer, descSet, binding);
    if (!node) {
      // We did not find the resource node. Return an undef value.
      return UndefValue::get(getTexelBufferDescPtrTy());
    }
  }

  // Get the descriptor pointer and stride as a struct.
  return getDescPtrAndStride(ResourceNodeType::DescriptorTexelBuffer, descSet, binding, topNode, node,
                             /*shadow=*/false);
}

// =====================================================================================================================
// Create a pointer to F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateGetFmaskDescPtr(unsigned descSet, unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  bool shadow = m_pipelineState->getOptions().shadowDescriptorTable != ShadowDescriptorTableDisable;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorFmask, descSet, binding);
    if (!node && shadow) {
      // For fmask with -enable-shadow-descriptor-table, if no fmask descriptor is found, look for a resource
      // (image) one instead.
      std::tie(topNode, node) =
          m_pipelineState->findResourceNode(ResourceNodeType::DescriptorResource, descSet, binding);
    }
    if (!node) {
      // We did not find the resource node. Return an undef value.
      return UndefValue::get(getSamplerDescPtrTy());
    }
  }

  // Get the descriptor pointer and stride as a struct.
  return getDescPtrAndStride(ResourceNodeType::DescriptorFmask, descSet, binding, topNode, node, shadow);
}

// =====================================================================================================================
// Create a load of the push constants table pointer.
// This returns a pointer to the ResourceNodeType::PushConst resource in the top-level user data table.
// The type passed must have the correct size for the push constants.
//
// @param pushConstantsTy : Type of the push constants table that the returned pointer will point to
// @param instName : Name to give instruction(s)
Value *DescBuilder::CreateLoadPushConstantsPtr(Type *pushConstantsTy, const Twine &instName) {
  // Get the push const pointer. If subsequent code only uses this with constant GEPs and loads,
  // then PatchEntryPointMutate might be able to "unspill" it so the code uses shader entry SGPRs
  // directly instead of loading from the spill table.
  Type *returnTy = pushConstantsTy->getPointerTo(ADDR_SPACE_CONST);
  std::string callName = lgcName::PushConst;
  addTypeMangling(returnTy, {}, callName);
  return CreateNamedCall(callName, returnTy, {}, Attribute::ReadOnly, instName);
}

// =====================================================================================================================
// Get a struct containing the pointer and byte stride for a descriptor
//
// @param resType : Resource type
// @param descSet : Descriptor set
// @param binding : Binding
// @param topNode : Node in top-level descriptor table (nullptr for shader compilation)
// @param node : The descriptor node itself (nullptr for shader compilation)
// @param shadow : Whether to load from shadow descriptor table
Value *DescBuilder::getDescPtrAndStride(ResourceNodeType resType, unsigned descSet, unsigned binding,
                                        const ResourceNode *topNode, const ResourceNode *node, bool shadow) {
  // Determine the stride if possible without looking at the resource node.
  const GpuProperty &gpuProperty = getLgcContext()->getTargetInfo().getGpuProperty();
  unsigned byteSize = 0;
  Value *stride = nullptr;
  switch (resType) {
  case ResourceNodeType::DescriptorBuffer:
  case ResourceNodeType::DescriptorTexelBuffer:
    byteSize = gpuProperty.descriptorSizeBuffer;
    if (node && node->type == ResourceNodeType::DescriptorBufferCompact)
      byteSize = DescriptorSizeBufferCompact;
    stride = getInt32(byteSize);
    break;
  case ResourceNodeType::DescriptorBufferCompact:
    byteSize = DescriptorSizeBufferCompact;
    stride = getInt32(byteSize);
    break;
  case ResourceNodeType::DescriptorSampler:
    byteSize = gpuProperty.descriptorSizeSampler;
    break;
  case ResourceNodeType::DescriptorResource:
  case ResourceNodeType::DescriptorFmask:
    byteSize = gpuProperty.descriptorSizeResource;
    break;
  default:
    llvm_unreachable("");
    break;
  }

  if (!stride) {
    // Stride is not determinable just from the descriptor type requested by the Builder call.
    if (m_pipelineState->isUnlinked() && m_pipelineState->getUserDataNodes().empty()) {
      // Shader compilation: Get byte stride using a reloc.
      stride = CreateRelocationConstant(reloc::DescriptorStride + Twine(descSet) + "_" + Twine(binding));
    } else {
      // Pipeline compilation: Get the stride from the resource type in the node.
      switch (node->type) {
      case ResourceNodeType::DescriptorSampler:
        stride = getInt32(gpuProperty.descriptorSizeSampler);
        break;
      case ResourceNodeType::DescriptorResource:
      case ResourceNodeType::DescriptorFmask:
        stride = getInt32(gpuProperty.descriptorSizeResource);
        break;
      case ResourceNodeType::DescriptorCombinedTexture:
      case ResourceNodeType::DescriptorYCbCrSampler:
        stride = getInt32(gpuProperty.descriptorSizeResource + gpuProperty.descriptorSizeSampler);
        break;
      default:
        llvm_unreachable("Unexpected resource node type");
        break;
      }
    }
  }

  Value *descPtr = nullptr;
  if (node && node->immutableValue && resType == ResourceNodeType::DescriptorSampler) {
    // This is an immutable sampler. Put the immutable value into a static variable and return a pointer
    // to that. For a simple non-variably-indexed immutable sampler not passed through a function call
    // or phi node, we rely on subsequent LLVM optimizations promoting the value back to a constant.
    StringRef startGlobalName = lgcName::ImmutableSamplerGlobal;
    // We need to change the stride to 4 dwords (8 dwords for a converting sampler). It would otherwise be
    // incorrectly set to 12 dwords for a sampler in a combined texture.
    stride = getInt32(gpuProperty.descriptorSizeSampler);
    if (node->type == ResourceNodeType::DescriptorYCbCrSampler) {
      startGlobalName = lgcName::ImmutableConvertingSamplerGlobal;
      stride = getInt32(DescriptorSizeSamplerYCbCr);
    }

    std::string globalName = (startGlobalName + Twine(node->set) + "_" + Twine(node->binding)).str();
    Module *module = GetInsertPoint()->getModule();
    descPtr = module->getGlobalVariable(globalName, /*AllowInternal=*/true);
    if (!descPtr) {
      descPtr = new GlobalVariable(*module, node->immutableValue->getType(),
                                   /*isConstant=*/true, GlobalValue::InternalLinkage, node->immutableValue, globalName,
                                   nullptr, GlobalValue::NotThreadLocal, ADDR_SPACE_CONST);
    }
    descPtr = CreateBitCast(descPtr, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));
  } else {
    // Get a pointer to the descriptor.
    descPtr = getDescPtr(resType, descSet, binding, topNode, node, shadow);
  }

  // Cast the pointer to the right type and create and return the struct.
  descPtr = CreateBitCast(descPtr, VectorType::get(getInt32Ty(), byteSize / 4)->getPointerTo(ADDR_SPACE_CONST));
  Value *descPtrStruct =
      CreateInsertValue(UndefValue::get(StructType::get(getContext(), {descPtr->getType(), getInt32Ty()})), descPtr, 0);
  descPtrStruct = CreateInsertValue(descPtrStruct, stride, 1);
  return descPtrStruct;
}

// =====================================================================================================================
// Get a pointer to a descriptor, as a pointer to i8
//
// @param resType : Resource type
// @param descSet : Descriptor set
// @param binding : Binding
// @param topNode : Node in top-level descriptor table (nullptr for shader compilation)
// @param node : The descriptor node itself (nullptr for shader compilation)
// @param shadow : Whether to load from shadow descriptor table
Value *DescBuilder::getDescPtr(ResourceNodeType resType, unsigned descSet, unsigned binding,
                               const ResourceNode *topNode, const ResourceNode *node, bool shadow) {
  Value *descPtr = nullptr;

  // Get the descriptor table pointer.
  // TODO Shader compilation: If we do not have user data layout info (topNode and node are nullptr), then
  // we do not know at compile time whether a DescriptorBuffer is in the root table or the table for its
  // descriptor set, so we need to generate a select between the two, where the condition is a reloc.
  if (node && node == topNode) {
    // The descriptor is in the top-level table. (This can only happen for a DescriptorBuffer.) Contrary
    // to what used to happen, we just load from the spill table, so we can get a pointer to the descriptor.
    // The spill table gets returned as a pointer to array of i8.
    descPtr =
        CreateNamedCall(lgcName::SpillTable, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST), {}, Attribute::ReadNone);
    // Ensure we mark spill table usage.
    getPipelineState()->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
  } else {
    // Get the descriptor table pointer for the set, which might be passed as a user SGPR to the shader.
    // The args to the lgc.descriptor.set call are:
    // - descriptor set number
    // - value for high 32 bits of pointer; HighAddrPc to use PC
    // TODO Shader compilation: For the "shadow" case, the high half of the address needs to be a reloc.
    unsigned highHalf = shadow ? m_pipelineState->getOptions().shadowDescriptorTable : HighAddrPc;
    descPtr = CreateNamedCall(lgcName::DescriptorSet, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST),
                              {getInt32(descSet), getInt32(highHalf)}, Attribute::ReadNone);
  }

  // Add on the byte offset of the descriptor.
  Value *offset = nullptr;
  if (!node) {
    // Shader compilation with no user data layout. Get the offset for the descriptor using a reloc. The
    // reloc symbol name needs to contain the descriptor set and binding, and, for image, fmask or sampler,
    // whether it is a sampler.
    StringRef relocNameSuffix = "";
    switch (resType) {
    case ResourceNodeType::DescriptorSampler:
    case ResourceNodeType::DescriptorYCbCrSampler:
      relocNameSuffix = "_s";
      break;
    case ResourceNodeType::DescriptorResource:
      relocNameSuffix = "_r";
      break;
    case ResourceNodeType::DescriptorBuffer:
    case ResourceNodeType::DescriptorBufferCompact:
    case ResourceNodeType::DescriptorTexelBuffer:
      relocNameSuffix = "_b";
      break;
    default:
      relocNameSuffix = "_x";
      break;
    }
    offset =
        CreateRelocationConstant(reloc::DescriptorOffset + Twine(descSet) + "_" + Twine(binding) + relocNameSuffix);
  } else {
    // Get the offset for the descriptor. Where we are getting the second part of a combined resource,
    // add on the size of the first part.
    const GpuProperty &gpuProperty = getLgcContext()->getTargetInfo().getGpuProperty();
    unsigned offsetInBytes = node->offsetInDwords * 4;
    if (resType == ResourceNodeType::DescriptorSampler && node->type == ResourceNodeType::DescriptorCombinedTexture)
      offsetInBytes += gpuProperty.descriptorSizeResource;
    offset = getInt32(offsetInBytes);
  }
  descPtr = CreateAddByteOffset(descPtr, offset);

  return descPtr;
}

// =====================================================================================================================
// Scalarize a value (pass it through readfirstlane) if uniform
//
// @param value : 32-bit integer value to scalarize
// @param isNonUniform : Whether value is marked as non-uniform
Value *DescBuilder::scalarizeIfUniform(Value *value, bool isNonUniform) {
  assert(value->getType()->isIntegerTy(32));
  if (!isNonUniform && !isa<Constant>(value)) {
    // NOTE: GFX6 encounters GPU hang with this optimization enabled. So we should skip it.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major > 6)
      value = CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, value);
  }
  return value;
}

// =====================================================================================================================
// Create a buffer length query based on the specified descriptor.
//
// @param bufferDesc : The buffer descriptor to query.
// @param instName : Name to give instruction(s).
Value *DescBuilder::CreateGetBufferDescLength(Value *const bufferDesc, Value *offset, const Twine &instName) {
  // In future this should become a full LLVM intrinsic, but for now we patch in a late intrinsic that is cleaned up
  // in patch buffer op.
  std::string callName = lgcName::LateBufferLength;
  return CreateNamedCall(lgcName::LateBufferLength, getInt32Ty(), {bufferDesc, offset}, Attribute::ReadNone);
}

// =====================================================================================================================
// Calculate a buffer descriptor for an inline buffer
//
// @param descPtr : Pointer to inline buffer
Value *DescBuilder::buildInlineBufferDesc(Value *descPtr) {
  // Bitcast the pointer to v2i32
  descPtr = CreatePtrToInt(descPtr, getInt64Ty());
  descPtr = CreateBitCast(descPtr, VectorType::get(getInt32Ty(), 2));

  // Build descriptor words.
  SqBufRsrcWord1 sqBufRsrcWord1 = {};
  SqBufRsrcWord2 sqBufRsrcWord2 = {};
  SqBufRsrcWord3 sqBufRsrcWord3 = {};

  sqBufRsrcWord1.bits.baseAddressHi = UINT16_MAX;
  sqBufRsrcWord2.bits.numRecords = UINT32_MAX;

  sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
  sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
  sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
  sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
  sqBufRsrcWord3.gfx6.numFormat = BUF_NUM_FORMAT_UINT;
  sqBufRsrcWord3.gfx6.dataFormat = BUF_DATA_FORMAT_32;
  assert(sqBufRsrcWord3.u32All == 0x24FAC);

  // Dword 0
  Value *desc = UndefValue::get(VectorType::get(getInt32Ty(), 4));
  Value *descElem0 = CreateExtractElement(descPtr, uint64_t(0));
  desc = CreateInsertElement(desc, descElem0, uint64_t(0));

  // Dword 1
  Value *descElem1 = CreateExtractElement(descPtr, 1);
  descElem1 = CreateAnd(descElem1, getInt32(sqBufRsrcWord1.u32All));
  desc = CreateInsertElement(desc, descElem1, 1);

  // Dword 2
  desc = CreateInsertElement(desc, getInt32(sqBufRsrcWord2.u32All), 2);

  // Dword 3
  desc = CreateInsertElement(desc, getInt32(sqBufRsrcWord3.u32All), 3);

  return desc;
}

// =====================================================================================================================
// Build buffer compact descriptor
//
// @param desc : The buffer descriptor base to build for the buffer compact descriptor
Value *DescBuilder::buildBufferCompactDesc(Value *desc) {
  // Extract compact buffer descriptor
  Value *descElem0 = CreateExtractElement(desc, uint64_t(0));
  Value *descElem1 = CreateExtractElement(desc, 1);

  // Build normal buffer descriptor
  // Dword 0
  Value *bufDesc = UndefValue::get(VectorType::get(getInt32Ty(), 4));
  bufDesc = CreateInsertElement(bufDesc, descElem0, uint64_t(0));

  // Dword 1
  SqBufRsrcWord1 sqBufRsrcWord1 = {};
  sqBufRsrcWord1.bits.baseAddressHi = UINT16_MAX;
  descElem1 = CreateAnd(descElem1, getInt32(sqBufRsrcWord1.u32All));
  bufDesc = CreateInsertElement(bufDesc, descElem1, 1);

  // Dword 2
  SqBufRsrcWord2 sqBufRsrcWord2 = {};
  sqBufRsrcWord2.bits.numRecords = UINT32_MAX;
  bufDesc = CreateInsertElement(bufDesc, getInt32(sqBufRsrcWord2.u32All), 2);

  // Dword 3
  const GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major < 10) {
    SqBufRsrcWord3 sqBufRsrcWord3 = {};
    sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
    sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
    sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
    sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
    sqBufRsrcWord3.gfx6.numFormat = BUF_NUM_FORMAT_UINT;
    sqBufRsrcWord3.gfx6.dataFormat = BUF_DATA_FORMAT_32;
    assert(sqBufRsrcWord3.u32All == 0x24FAC);
    bufDesc = CreateInsertElement(bufDesc, getInt32(sqBufRsrcWord3.u32All), 3);
  } else if (gfxIp.major == 10) {
    SqBufRsrcWord3 sqBufRsrcWord3 = {};
    sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
    sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
    sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
    sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
    sqBufRsrcWord3.gfx10.format = BUF_FORMAT_32_UINT;
    sqBufRsrcWord3.gfx10.resourceLevel = 1;
    sqBufRsrcWord3.gfx10.oobSelect = 2;
    assert(sqBufRsrcWord3.u32All == 0x21014FAC);
    bufDesc = CreateInsertElement(bufDesc, getInt32(sqBufRsrcWord3.u32All), 3);
  } else {
    llvm_unreachable("Not implemented!");
  }
  return bufDesc;
}
