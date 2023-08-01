/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-builder-impl-desc"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a load of a buffer descriptor.
//
// If descSet = InternalDescriptorSetId (0xFFFFFFFF), this is an internal user data, which is a plain 64-bit pointer,
// flags must be 'BufferFlagAddress' i64 address is returned.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param descIndex : Descriptor index
// @param flags : BufferFlag* bit settings
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateLoadBufferDesc(uint64_t descSet, unsigned binding, Value *descIndex, unsigned flags,
                                         const Twine &instName) {
  Value *desc = nullptr;
  bool return64Address = false;
  if (flags & BufferFlagAddress)
    return64Address = true;

  desc = CreateBufferDesc(descSet, binding, descIndex, flags, instName);
  if (return64Address || isa<PoisonValue>(desc))
    return desc;

  // Convert to fat pointer.
  return create<BufferDescToPtrOp>(desc);
}

// =====================================================================================================================
// Create a buffer descriptor, not convert to a fat pointer
//
// If descSet = -1, this is an internal user data, which is a plain 64-bit pointer, flags must be 'BufferFlagAddress'
// i64 address is returned.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param descIndex : Descriptor index
// @param flags : BufferFlag* bit settings
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateBufferDesc(uint64_t descSet, unsigned binding, Value *descIndex, unsigned flags,
                                     const Twine &instName) {
  Value *desc = nullptr;
  bool return64Address = false;
  descIndex = scalarizeIfUniform(descIndex, flags & BufferFlagNonUniform);

  // Mark the shader as reading and writing (if applicable) a resource.
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
  resUsage->resourceRead = true;
  if (flags & BufferFlagWritten)
    resUsage->resourceWrite = true;
  else if (flags & BufferFlagAddress)
    return64Address = true;

  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look. Later code will use relocs.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    // We have the user data layout. Find the node.
    ResourceNodeType abstractType = ResourceNodeType::Unknown;
    if (flags & BufferFlagConst)
      abstractType = ResourceNodeType::DescriptorConstBuffer;
    else if (flags & BufferFlagNonConst)
      abstractType = ResourceNodeType::DescriptorBuffer;
    else if (flags & BufferFlagShaderResource)
      abstractType = ResourceNodeType::DescriptorResource;
    else if (flags & BufferFlagSampler)
      abstractType = ResourceNodeType::DescriptorSampler;
    else if (flags & BufferFlagAddress)
      abstractType = ResourceNodeType::DescriptorBufferCompact;

    std::tie(topNode, node) = m_pipelineState->findResourceNode(abstractType, descSet, binding, m_shaderStage);
    if (!node) {
      // If we can't find the node, assume mutable descriptor and search for any node.
      std::tie(topNode, node) =
          m_pipelineState->findResourceNode(ResourceNodeType::DescriptorMutable, descSet, binding, m_shaderStage);
    }

    if (!node)
      report_fatal_error("Resource node not found");

    if (node == topNode && isa<Constant>(descIndex) && node->concreteType != ResourceNodeType::InlineBuffer) {
      // Handle a descriptor in the root table (a "dynamic descriptor") specially, as long as it is not variably
      // indexed and is not an InlineBuffer.
      Type *descTy;
      if (return64Address) {
        assert(node->concreteType == ResourceNodeType::DescriptorBufferCompact);
        descTy = getInt64Ty();
      } else {
        descTy = getDescTy(node->concreteType);
      }

      unsigned dwordSize = descTy->getPrimitiveSizeInBits() / 32;
      unsigned dwordOffset = cast<ConstantInt>(descIndex)->getZExtValue() * dwordSize;
      if (dwordOffset + dwordSize > node->sizeInDwords) {
        // Index out of range
        desc = PoisonValue::get(descTy);
      } else {
        dwordOffset += node->offsetInDwords;
        dwordOffset += (binding - node->binding) * node->stride;
        desc = create<LoadUserDataOp>(descTy, dwordOffset * 4);
      }
      if (return64Address)
        return desc;
    } else if (node->concreteType == ResourceNodeType::InlineBuffer) {
      // Handle an inline buffer specially. Get a pointer to it, then expand to a descriptor.
      Value *descPtr = getDescPtr(node->concreteType, node->abstractType, descSet, binding, topNode, node);
      desc = buildInlineBufferDesc(descPtr);
    }
  }

  if (!desc) {
    if (node) {
      ResourceNodeType resType = node->concreteType;
      ResourceNodeType abstractType = node->abstractType;
      // Handle mutable descriptors
      if (resType == ResourceNodeType::DescriptorMutable) {
        resType = ResourceNodeType::DescriptorBuffer;
      }
      if (abstractType == ResourceNodeType::DescriptorMutable) {
        abstractType = ResourceNodeType::DescriptorBuffer;
      }
      Value *descPtr = getDescPtr(resType, abstractType, descSet, binding, topNode, node);
      // Index it.
      if (descIndex != getInt32(0)) {
        descIndex = CreateMul(descIndex, getStride(resType, descSet, binding, node));
        descPtr = CreateGEP(getInt8Ty(), descPtr, descIndex);
      }

      // The buffer may have an attached counter buffer descriptor which do not have a different set or binding.
      if (flags & BufferFlagAttachedCounter) {
        // The node stride must be large enough to hold 2 buffer descriptors.
        assert(node->stride * sizeof(uint32_t) == 2 * DescriptorSizeBuffer);
        descPtr = CreateGEP(getInt8Ty(), descPtr, getInt32(DescriptorSizeBuffer));
      }

      // Cast it to the right type.
      descPtr = CreateBitCast(descPtr, getDescPtrTy(resType));
      // Load the descriptor.
      desc = CreateLoad(getDescTy(resType), descPtr);
    } else {
      // For shader compilation with no user data layout provided, we don't know if the buffer is dynamic descriptor,
      // We need to load two dwords for DescriptorBufferCompact, 4 dwords for DescriptorBuffer. To avoid out of bound,
      // we will use two loads and load two dwords for each time. If the resource type is really DescriptorBuffer, the
      // address of the second load will add 8 bytes, otherwise the address is the same as the first, it means we load
      // the same data twice, but the data is not used.

      // Get the descriptor pointer which is from ResourceMapping, ignore the resource type.
      ResourceNodeType resType = ResourceNodeType::DescriptorBuffer;
      ResourceNodeType abstractType = resType;
      Value *descPtr = getDescPtr(resType, abstractType, descSet, binding, nullptr, nullptr);
      // Index it.
      if (descIndex != getInt32(0)) {
        descIndex = CreateMul(descIndex, getStride(resType, descSet, binding, nullptr));
        descPtr = CreateGEP(getInt8Ty(), descPtr, descIndex);
      }

      auto descPtrLo = CreateBitCast(descPtr, FixedVectorType::get(getInt32Ty(), 2)->getPointerTo(ADDR_SPACE_CONST));
      // The first load
      auto descLo = CreateLoad(FixedVectorType::get(getInt32Ty(), 2), descPtrLo);
      auto compactBufferDesc = buildBufferCompactDesc(descLo);

      // If descriptor set is InternalDescriptorSetId, this is a internal resource node, it is a root node
      // and its type is ResourceNodeType::DescriptorBufferCompact.
      if (descSet == InternalDescriptorSetId) {
        assert(return64Address);
        return CreateBitCast(descLo, getInt64Ty());
      } else {
        // Add offset
        Value *descPtrHi = CreateAddByteOffset(descPtr, getInt32(8));
        auto reloc = CreateRelocationConstant(reloc::CompactBuffer + Twine(descSet) + "_" + Twine(binding));
        auto isCompactBuffer = CreateICmpNE(reloc, getInt32(0));
        // Select the address
        descPtrHi = CreateSelect(isCompactBuffer, descPtr, descPtrHi);
        descPtrHi = CreateBitCast(descPtrHi, FixedVectorType::get(getInt32Ty(), 2)->getPointerTo(ADDR_SPACE_CONST));
        // The second load
        auto descHi = CreateLoad(FixedVectorType::get(getInt32Ty(), 2), descPtrHi);
        // Merge the whole descriptor for DescriptorBuffer
        auto bufferDesc = CreateShuffleVector(descLo, descHi, {0, 1, 2, 3});
        // Select
        desc = CreateSelect(isCompactBuffer, compactBufferDesc, bufferDesc);
      }
    }
  }
  if (node && (node->concreteType == ResourceNodeType::DescriptorBufferCompact ||
               node->concreteType == ResourceNodeType::DescriptorConstBufferCompact))
    desc = buildBufferCompactDesc(desc);

  if (!instName.isTriviallyEmpty())
    desc->setName(instName);

  return desc;
}

// =====================================================================================================================
// Create a get of the stride (in bytes) of a descriptor. Returns an i32 value.
//
// @param concreteType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param abstractType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateGetDescStride(ResourceNodeType concreteType, ResourceNodeType abstractType, uint64_t descSet,
                                        unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look; we will use relocs instead.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(abstractType, descSet, binding, m_shaderStage);
    if (!node) {
      // If we can't find the node, assume mutable descriptor and search for any node.
      std::tie(topNode, node) =
          m_pipelineState->findResourceNode(ResourceNodeType::DescriptorMutable, descSet, binding, m_shaderStage);
      if (!node &&
          m_pipelineState->findResourceNode(ResourceNodeType::Unknown, descSet, binding, m_shaderStage).second) {
        // NOTE: Resource node may be DescriptorTexelBuffer, but it is defined as OpTypeSampledImage in SPIRV,
        // In this case, a caller may search for the DescriptorSampler and not find it. We return nullptr and
        // expect the caller to handle it.
        return PoisonValue::get(getInt32Ty());
      }
      assert(node && "missing resource node");
    }
    assert(node && "missing resource node");
  }
  return getStride(concreteType, descSet, binding, node);
}

// =====================================================================================================================
// Create a pointer to a descriptor. Returns a value of the type returned by GetSamplerDescPtrTy, GetImageDescPtrTy,
// GetTexelBufferDescPtrTy or GetFmaskDescPtrTy, depending on descType.
//
// @param concreteType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param abstractType : Descriptor type to find user resource nodes;
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateGetDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, uint64_t descSet,
                                     unsigned binding, const Twine &instName) {
  // Find the descriptor node. If doing a shader compilation with no user data layout provided, don't bother to
  // look; we will use relocs instead.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(abstractType, descSet, binding, m_shaderStage);
    if (!node) {
      // If we can't find the node, assume mutable descriptor and search for any node.
      std::tie(topNode, node) =
          m_pipelineState->findResourceNode(ResourceNodeType::DescriptorMutable, descSet, binding, m_shaderStage);
      if (!node &&
          m_pipelineState->findResourceNode(ResourceNodeType::Unknown, descSet, binding, m_shaderStage).second) {
        // NOTE: Resource node may be DescriptorTexelBuffer, but it is defined as OpTypeSampledImage in SPIRV,
        // In this case, a caller may search for the DescriptorSampler and not find it. We return nullptr and
        // expect the caller to handle it.
        return PoisonValue::get(getDescPtrTy(concreteType));
      }
      assert(node && "missing resource node");
    }
  }

  Value *descPtr = nullptr;
  if (node && node->immutableSize != 0 && concreteType == ResourceNodeType::DescriptorSampler) {
    // This is an immutable sampler. Put the immutable value into a static variable and return a pointer
    // to that. For a simple non-variably-indexed immutable sampler not passed through a function call
    // or phi node, we rely on subsequent LLVM optimizations promoting the value back to a constant.
    StringRef startGlobalName = lgcName::ImmutableSamplerGlobal;
    std::string globalName = (startGlobalName + Twine(node->set) + "_" + Twine(node->binding)).str();
    Module *module = GetInsertPoint()->getModule();
    descPtr = module->getGlobalVariable(globalName, /*AllowInternal=*/true);
    if (!descPtr) {
      // We need to create the global variable as it does not already exist.
      // First construct the immutable value as an array of <4 x i32>, for use as the initializer of the
      // global variable.
      SmallVector<Constant *> immutableArray;
      for (unsigned idx = 0; idx != node->immutableSize; ++idx) {
        Constant *dwords[DescriptorSizeSamplerInDwords];
        for (unsigned subidx = 0; subidx != DescriptorSizeSamplerInDwords; ++subidx)
          dwords[subidx] = getInt32(node->immutableValue[idx * DescriptorSizeSamplerInDwords + subidx]);
        immutableArray.push_back(ConstantVector::get(dwords));
      }
      Constant *globalInit =
          ConstantArray::get(ArrayType::get(immutableArray[0]->getType(), immutableArray.size()), immutableArray);
      // Then create the global variable.
      descPtr = new GlobalVariable(*module, globalInit->getType(),
                                   /*isConstant=*/true, GlobalValue::InternalLinkage, globalInit, globalName, nullptr,
                                   GlobalValue::NotThreadLocal, ADDR_SPACE_CONST);
    }
  } else {
    // Get a pointer to the descriptor.
    descPtr = getDescPtr(concreteType, abstractType, descSet, binding, topNode, node);
  }

  // Cast to the right pointer type.
  return CreateBitCast(descPtr, getDescPtrTy(concreteType));
}

// =====================================================================================================================
// Create a load of the push constants table pointer.
// This returns a pointer to the ResourceNodeType::PushConst resource in the top-level user data table.
// The type passed must have the correct size for the push constants.
//
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateLoadPushConstantsPtr(const Twine &instName) {
  Value *ptr;
  const ResourceNode *topNode = m_pipelineState->findPushConstantResourceNode(m_shaderStage);
  assert(topNode);
  if (topNode->concreteType == ResourceNodeType::DescriptorTableVaPtr) {
    const ResourceNode subNode = topNode->innerTable[0];
    Value *highHalf = getInt32(HighAddrPc);
    ptr = CreateNamedCall(lgcName::DescriptorTableAddr, getPtrTy(ADDR_SPACE_CONST),
                          {getInt32(unsigned(ResourceNodeType::PushConst)),
                           getInt32(unsigned(ResourceNodeType::PushConst)), getInt64(subNode.set),
                           getInt32(subNode.binding), highHalf},
                          Attribute::ReadNone);
  } else {
    assert(topNode->concreteType == ResourceNodeType::PushConst);
    ptr = create<UserDataOp>(topNode->offsetInDwords * 4);
  }
  ptr->setName(instName);
  return ptr;
}

// =====================================================================================================================
// Check whether vertex buffer descriptors are in a descriptor array binding instead of the VertexBufferTable
bool BuilderImpl::useVertexBufferDescArray() {
  return m_pipelineState->getInputAssemblyState().useVertexBufferDescArray == 1;
}

// =====================================================================================================================
// Get the stride (in bytes) of a descriptor. Returns an i32 value.
//
// @param descType : Descriptor type
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param node : The descriptor node (nullptr for shader compilation)
// @param instName : Name to give instruction(s)
Value *BuilderImpl::getStride(ResourceNodeType descType, uint64_t descSet, unsigned binding, const ResourceNode *node) {
  if (node && node->immutableSize != 0 && descType == ResourceNodeType::DescriptorSampler) {
    // This is an immutable sampler. Because we put the immutable value into a static variable, the stride is
    // always the size of the descriptor.
    return getInt32(DescriptorSizeSampler);
  }

  if (m_pipelineState->isUnlinked() && m_pipelineState->getUserDataNodes().empty()) {
    // Shader compilation: Get byte stride using a reloc.
    return CreateRelocationConstant(reloc::DescriptorStride + Twine(descSet) + "_" + Twine(binding));
  }
  // Pipeline compilation: Get the stride from the node.
  assert(node);
  return getInt32(node->stride * sizeof(uint32_t));
}

// =====================================================================================================================
// Get a pointer to a descriptor, as a pointer to i8
//
// @param concreteType : Concrete resource type
// @param abstractType : Abstract Resource type
// @param descSet : Descriptor set
// @param binding : Binding
// @param topNode : Node in top-level descriptor table (nullptr for shader compilation)
// @param node : The descriptor node itself (nullptr for shader compilation)
Value *BuilderImpl::getDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, uint64_t descSet,
                               unsigned binding, const ResourceNode *topNode, const ResourceNode *node) {
  auto GetDescriptorSetPtr = [this, node, topNode, concreteType, abstractType, descSet, binding]() -> Value * {
    // Get the descriptor table pointer for the descriptor at the given set and binding, which might be passed as a
    // user SGPR to the shader.
    // The args to the lgc.descriptor.table.addr call are:
    // - requested descriptor type
    // - descriptor set number
    // - descriptor binding number
    // - value for high 32 bits of the pointer; HighAddrPc to use PC
    if (node || topNode || concreteType != ResourceNodeType::DescriptorFmask) {
      unsigned highAddrOfFmask = m_pipelineState->getOptions().highAddrOfFmask;
      bool isFmask = concreteType == ResourceNodeType::DescriptorFmask;
      assert(!(isFmask && highAddrOfFmask == ShadowDescriptorTableDisable) && "not implemented");
      Value *highHalf = getInt32(isFmask ? highAddrOfFmask : HighAddrPc);
      return CreateNamedCall(lgcName::DescriptorTableAddr, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST),
                             {getInt32(unsigned(concreteType)), getInt32(unsigned(abstractType)), getInt64(descSet),
                              getInt32(binding), highHalf},
                             Attribute::ReadNone);
    }
    // This should be an unlinked shader, and we will use a relocation for the high half of the address.
    assert(m_pipelineState->isUnlinked() &&
           "Cannot add shadow descriptor relocations unless building an unlinked shader.");

    // Get the address when the shadow table is disabled.
    Value *nonShadowAddr = CreateNamedCall(lgcName::DescriptorTableAddr, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST),
                                           {getInt32(unsigned(concreteType)), getInt32(unsigned(abstractType)),
                                            getInt64(descSet), getInt32(binding), getInt32(HighAddrPc)},
                                           Attribute::ReadNone);

    // Get the address using a relocation when the shadow table is enabled.
    Value *shadowDescriptorReloc = CreateRelocationConstant(reloc::ShadowDescriptorTable);
    Value *shadowAddr = CreateNamedCall(lgcName::DescriptorTableAddr, getInt8Ty()->getPointerTo(ADDR_SPACE_CONST),
                                        {getInt32(unsigned(concreteType)), getInt32(unsigned(abstractType)),
                                         getInt64(descSet), getInt32(binding), shadowDescriptorReloc},
                                        Attribute::ReadNone);

    // Use a relocation to select between the two.
    Value *useShadowReloc = CreateRelocationConstant(reloc::ShadowDescriptorTableEnabled);
    Value *useShadowTable = CreateICmpNE(useShadowReloc, getInt32(0));
    return CreateSelect(useShadowTable, shadowAddr, nonShadowAddr);
  };

  // Get the offset for the descriptor. Where we are getting the second part of a combined resource,
  // add on the size of the first part.
  unsigned offsetInDwords = node->offsetInDwords + (binding - node->binding) * node->stride;
  unsigned offsetInBytes = offsetInDwords * 4;
  if (concreteType == ResourceNodeType::DescriptorSampler &&
      node->concreteType == ResourceNodeType::DescriptorCombinedTexture)
    offsetInBytes += DescriptorSizeResource;

  if (node && node == topNode)
    return create<UserDataOp>(offsetInBytes);

  Value *descPtr = GetDescriptorSetPtr();
  return CreateConstGEP1_32(getInt8Ty(), descPtr, offsetInBytes);
}

// =====================================================================================================================
// Scalarize a value (pass it through readfirstlane) if uniform
//
// @param value : 32-bit integer value to scalarize
// @param isNonUniform : Whether value is marked as non-uniform
Value *BuilderImpl::scalarizeIfUniform(Value *value, bool isNonUniform) {
  assert(value->getType()->isIntegerTy(32));
  if (!isNonUniform && !isa<Constant>(value)) {
    // NOTE: GFX6 encounters GPU hang with this optimization enabled. So we should skip it.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major > 6)
      value = CreateIntrinsic(getInt32Ty(), Intrinsic::amdgcn_readfirstlane, value);
  }
  return value;
}

// =====================================================================================================================
// Calculate a buffer descriptor for an inline buffer
//
// @param descPtr : Pointer to inline buffer
Value *BuilderImpl::buildInlineBufferDesc(Value *descPtr) {
  // Bitcast the pointer to v2i32
  descPtr = CreatePtrToInt(descPtr, getInt64Ty());
  descPtr = CreateBitCast(descPtr, FixedVectorType::get(getInt32Ty(), 2));

  return buildBufferCompactDesc(descPtr);
}

// =====================================================================================================================
// Build buffer compact descriptor
//
// @param desc : The buffer descriptor base to build for the buffer compact descriptor
Value *BuilderImpl::buildBufferCompactDesc(Value *desc) {
  const GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  // Extract compact buffer descriptor
  Value *descElem0 = CreateExtractElement(desc, uint64_t(0));
  Value *descElem1 = CreateExtractElement(desc, 1);

  // Build normal buffer descriptor
  // Dword 0
  Value *bufDesc = PoisonValue::get(FixedVectorType::get(getInt32Ty(), 4));
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
  SqBufRsrcWord3 sqBufRsrcWord3 = {};
  sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
  sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
  sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
  sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
  if (gfxIp.major < 10) {
    sqBufRsrcWord3.gfx6.numFormat = BUF_NUM_FORMAT_UINT;
    sqBufRsrcWord3.gfx6.dataFormat = BUF_DATA_FORMAT_32;
    assert(sqBufRsrcWord3.u32All == 0x24FAC);
  } else if (gfxIp.major == 10) {
    sqBufRsrcWord3.gfx10.format = BUF_FORMAT_32_UINT;
    sqBufRsrcWord3.gfx10.resourceLevel = 1;
    sqBufRsrcWord3.gfx10.oobSelect = 2;
    assert(sqBufRsrcWord3.u32All == 0x21014FAC);
  } else if (gfxIp.major >= 11) {
    sqBufRsrcWord3.gfx11.format = BUF_FORMAT_32_UINT;
    sqBufRsrcWord3.gfx11.oobSelect = 2;
    assert(sqBufRsrcWord3.u32All == 0x20014FAC);
  } else {
    llvm_unreachable("Not implemented!");
  }
  bufDesc = CreateInsertElement(bufDesc, getInt32(sqBufRsrcWord3.u32All), 3);

  return bufDesc;
}
