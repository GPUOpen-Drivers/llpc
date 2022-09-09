/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#include "SPIRVInternal.h"
#include "lgc/Builder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
// TODO: Fix the code in this file so it does not break the builder abstraction. It should
// not be including files directly in the LGC directory tree like this.
#include "../../lgc/include/lgc/builder/BuilderRecorder.h"
#include "llpcContext.h"
#include "llpcSpirvLowerResourceCollect.h"

#define DEBUG_TYPE "llpc-spirv-lower-resource-collect"

using namespace lgc;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Initializes static members.
char SpirvLowerResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for resource collecting
//
// @param collectDetailUsage : Whether to collect detailed usages of resource node datas and FS output infos
ModulePass *createSpirvLowerResourceCollect(bool collectDetailUsage) {
  return new SpirvLowerResourceCollect(collectDetailUsage);
}

// =====================================================================================================================
//
// @param collectDetailUsage : Whether to collect detailed usages of resource node datas and FS output infos
SpirvLowerResourceCollect::SpirvLowerResourceCollect(bool collectDetailUsage)
    : LegacySpirvLower(ID), m_collectDetailUsage(collectDetailUsage), m_detailUsageValid(false) {
}

// =====================================================================================================================
// Collect resource node data
//
// @param global : Global variable to collect resource node data
void SpirvLowerResourceCollect::collectResourceNodeData(const GlobalVariable *global) {
  auto globalTy = global->getValueType();

  MDNode *metaNode = global->getMetadata(gSPIRVMD::Resource);
  auto descSet = mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(0))->getZExtValue();
  auto binding = mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(1))->getZExtValue();
  auto spvOpCode = mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(2))->getZExtValue();

  // Map the SPIR-V opcode to descriptor type.
  ResourceMappingNodeType nodeType = ResourceMappingNodeType::Unknown;
  switch (spvOpCode) {
  case OpTypeSampler: {
    // Sampler descriptor.
    nodeType = ResourceMappingNodeType::DescriptorSampler;
    break;
  }
  case OpTypeImage: {
    nodeType = ResourceMappingNodeType::DescriptorResource;
    // Image descriptor.
    Type *imageType = globalTy->getPointerElementType();
    const std::string imageTypeName(imageType->getStructName());
    // Format of image opaque type: ...[.SampledImage.<date type><dim>]...
    if (imageTypeName.find(".SampledImage") != std::string::npos) {
      auto pos = imageTypeName.find('_');
      assert(pos != std::string::npos);

      ++pos;
      Dim dim = static_cast<Dim>(imageTypeName[pos] - '0');
      nodeType = dim == DimBuffer ? ResourceMappingNodeType::DescriptorTexelBuffer
                                  : ResourceMappingNodeType::DescriptorResource;
    }
    break;
  }
  case OpTypeSampledImage: {
    // Combined image and sampler descriptors.
    nodeType = ResourceMappingNodeType::DescriptorCombinedTexture;
    break;
  }
  default: {
    // Normal buffer.
    nodeType = ResourceMappingNodeType::DescriptorBuffer;
    break;
  }
  }

  ResourceNodeDataKey nodeData = {};

  nodeData.value.set = descSet;
  nodeData.value.binding = binding;
  nodeData.value.arraySize = getFlattenArrayElementCount(globalTy);
  auto result = m_resNodeDatas.insert(std::pair<ResourceNodeDataKey, ResourceMappingNodeType>(nodeData, nodeType));

  // Check if the node already had a different pair of node data/type. A DescriptorResource/DescriptorTexelBuffer
  // and a DescriptorSampler can use the same set/binding, in which case it is
  // DescriptorCombinedTexture.
  if (!result.second) {
    assert((nodeType == ResourceMappingNodeType::DescriptorCombinedTexture ||
            nodeType == ResourceMappingNodeType::DescriptorResource ||
            nodeType == ResourceMappingNodeType::DescriptorTexelBuffer ||
            nodeType == ResourceMappingNodeType::DescriptorSampler) &&
           (result.first->second == ResourceMappingNodeType::DescriptorCombinedTexture ||
            result.first->second == ResourceMappingNodeType::DescriptorResource ||
            result.first->second == ResourceMappingNodeType::DescriptorTexelBuffer ||
            result.first->second == ResourceMappingNodeType::DescriptorSampler));
    result.first->second = ResourceMappingNodeType::DescriptorCombinedTexture;
  }
}
// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerResourceCollect::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Resource-Collect\n");

  SpirvLower::init(&module);

  // Collect unused globals and remove them
  std::unordered_set<GlobalVariable *> removedGlobals;
  for (auto global = m_module->global_begin(), end = m_module->global_end(); global != end; ++global) {
    if (global->user_empty()) {
      Value *initializer = nullptr;
      if (global->hasInitializer())
        initializer = global->getInitializer();

      if (!initializer || isa<UndefValue>(initializer))
        removedGlobals.insert(&*global);
    }
  }

  for (auto global : removedGlobals) {
    global->dropAllReferences();
    global->eraseFromParent();
  }

  // Collect resource usages from globals
  for (auto global = m_module->global_begin(), end = m_module->global_end(); global != end; ++global) {
    auto addrSpace = global->getType()->getAddressSpace();
    switch (addrSpace) {
    case SPIRAS_Constant: {
      if (!global->hasMetadata(gSPIRVMD::PushConst)) {
        // Non push constant, only collect resource node data when requested
        if (m_collectDetailUsage)
          collectResourceNodeData(&*global);
      }
      break;
    }
    case SPIRAS_Private:
    case SPIRAS_Global:
    case SPIRAS_Local:
    case SPIRAS_Input: {
      break;
    }
    case SPIRAS_Output: {
      // Only collect FS out info when requested.
      Type *globalTy = global->getValueType();
      if (!m_collectDetailUsage || !globalTy->isSingleValueType())
        break;

      FsOutInfo fsOutInfo = {};
      MDNode *metaNode = global->getMetadata(gSPIRVMD::InOut);
      auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

      ShaderInOutMetadata inOutMeta = {};
      Constant *inOutMetaConst = cast<Constant>(meta);
      inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaConst->getOperand(0))->getZExtValue();
      inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaConst->getOperand(1))->getZExtValue();

      const unsigned location = inOutMeta.Value;
      const unsigned index = inOutMeta.Index;

      // Collect basic types of fragment outputs
      BasicType basicTy = BasicType::Unknown;

      const auto compTy = globalTy->isVectorTy() ? cast<VectorType>(globalTy)->getElementType() : globalTy;
      const unsigned bitWidth = compTy->getScalarSizeInBits();
      const bool signedness = (inOutMeta.Signedness != 0);

      if (compTy->isIntegerTy()) {
        // Integer type
        if (bitWidth == 8)
          basicTy = signedness ? BasicType::Int8 : BasicType::Uint8;
        else if (bitWidth == 16)
          basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
        else {
          assert(bitWidth == 32);
          basicTy = signedness ? BasicType::Int : BasicType::Uint;
        }
      } else if (compTy->isFloatingPointTy()) {
        // Floating-point type
        if (bitWidth == 16)
          basicTy = BasicType::Float16;
        else {
          assert(bitWidth == 32);
          basicTy = BasicType::Float;
        }
      } else
        llvm_unreachable("Should never be called!");

      fsOutInfo.location = location;
      fsOutInfo.index = index;
      fsOutInfo.componentCount = globalTy->isVectorTy() ? cast<FixedVectorType>(globalTy)->getNumElements() : 1;
      fsOutInfo.basicType = basicTy;
      m_fsOutInfos.push_back(fsOutInfo);
      break;
    }
    case SPIRAS_Uniform: {
      // Only collect resource node data when requested
      if (m_collectDetailUsage)
        collectResourceNodeData(&*global);
      break;
    }
    default: {
      llvm_unreachable("Should never be called!");
      break;
    }
    }
  }

  if (m_collectDetailUsage)
    visitCalls(module);
  if (!m_fsOutInfos.empty() || !m_resNodeDatas.empty())
    m_detailUsageValid = true;

  return true;
}

// =====================================================================================================================
// Gets element count if the specified type is an array (flattened for multi-dimension array).
//
// @param ty : Type to check
unsigned SpirvLowerResourceCollect::getFlattenArrayElementCount(const Type *ty) const {
  unsigned elemCount = 1;

  auto arrayTy = dyn_cast<ArrayType>(ty);
  while (arrayTy) {
    elemCount *= arrayTy->getArrayNumElements();
    arrayTy = dyn_cast<ArrayType>(arrayTy->getArrayElementType());
  }
  return elemCount;
}

// =====================================================================================================================
// Gets element type if the specified type is an array (flattened for multi-dimension array).
//
// @param ty : Type to check
const Type *SpirvLowerResourceCollect::getFlattenArrayElementType(const Type *ty) const {
  const Type *elemType = ty;

  auto arrayTy = dyn_cast<ArrayType>(ty);
  while (arrayTy) {
    elemType = arrayTy->getArrayElementType();
    arrayTy = dyn_cast<ArrayType>(elemType);
  }
  return elemType;
}

// =====================================================================================================================
// Find the specified target call and get the index value from corresponding argument
//
// @param module : LLVM module to be visited
// @param targetCall : Builder call as search target
Value *SpirvLowerResourceCollect::findCallAndGetIndexValue(Module &module, CallInst *const targetCall) {
  // TODO: This code no longer compiles, because BuilderRecorder::Opcode::IndexDescPtr no longer exists.
  // 1. The way it scans IR for LGC Builder calls is dodgy, because that breaks the Builder abstraction.
  // 2. I don't know why it is doing this loop to find a use of targetCall. Surely it should just be doing
  //    a loop on targetCall->users().
  // 3. The Builder API for indexing a descriptor pointer has changed -- now the SPIR-V reader just does its
  //    own regular IR, instead of calling a Builder method.
  // 4. I think this code is being called in the hope of finding an index operation on a particular descriptor
  //    pointer, for marking what size array the descriptor is. But what if the index is variable? What if the
  //    same descriptor is used more than once with different indices?
  // 5. I think all this code conditional on m_collectDetailUsage is experimental, and is not being used in anger
  //    in the driver.
#if 0
  for (auto &func : module) {
    // Skip non-declarations that are definitely not LLPC builder calls.
    if (!func.isDeclaration())
      continue;

    const MDNode *const funcMeta = func.getMetadata(module.getMDKindID(BuilderCallOpcodeMetadataName));

    // Skip builder calls that do not have the correct metadata to identify the opcode.
    if (!funcMeta) {
      // If the function had the LLPC builder call prefix, it means the metadata was not encoded correctly.
      assert(func.getName().startswith(BuilderCallPrefix) == false);
      continue;
    }

    const ConstantAsMetadata *const metaConst = cast<ConstantAsMetadata>(funcMeta->getOperand(0));
    unsigned opcode = cast<ConstantInt>(metaConst->getValue())->getZExtValue();

    if (opcode == BuilderRecorder::Opcode::IndexDescPtr) {
      for (auto useIt = func.use_begin(), useItEnd = func.use_end(); useIt != useItEnd; ++useIt) {
        CallInst *const call = dyn_cast<CallInst>(useIt->getUser());

        // Get the args.
        auto args = ArrayRef<Use>(&call->getOperandList()[0], call->arg_size());

        if (args[0] == targetCall)
          return args[1];
      }
    }
  }
#endif

  return nullptr;
}

// =====================================================================================================================
// Visit all LLPC builder calls in a module
//
// @param module : LLVM module to be visited
void SpirvLowerResourceCollect::visitCalls(Module &module) {
  for (auto &func : module) {
    // Skip non-declarations that are definitely not LLPC builder calls.
    if (!func.isDeclaration())
      continue;

    const MDNode *const funcMeta = func.getMetadata(module.getMDKindID(BuilderCallOpcodeMetadataName));

    // Skip builder calls that do not have the correct metadata to identify the opcode.
    if (!funcMeta) {
      // If the function had the llpc builder call prefix, it means the metadata was not encoded correctly.
      assert(func.getName().startswith(BuilderCallPrefix) == false);
      continue;
    }

    const ConstantAsMetadata *const metaConst = cast<ConstantAsMetadata>(funcMeta->getOperand(0));
    unsigned opcode = cast<ConstantInt>(metaConst->getValue())->getZExtValue();

    for (auto useIt = func.use_begin(), useItEnd = func.use_end(); useIt != useItEnd; ++useIt) {
      CallInst *const call = dyn_cast<CallInst>(useIt->getUser());

      // Get the args.
      auto args = ArrayRef<Use>(&call->getOperandList()[0], call->arg_size());

      ResourceMappingNodeType nodeType = ResourceMappingNodeType::Unknown;
      if (opcode == BuilderRecorder::Opcode::GetDescPtr)
        nodeType = static_cast<ResourceMappingNodeType>(cast<ConstantInt>(args[0])->getZExtValue());

      if (nodeType != ResourceMappingNodeType::Unknown) {
        ResourceNodeDataKey nodeData = {};

        nodeData.value.set = cast<ConstantInt>(args[1])->getZExtValue();
        nodeData.value.binding = cast<ConstantInt>(args[2])->getZExtValue();
        nodeData.value.arraySize = 1;
        auto index = findCallAndGetIndexValue(module, call);
        if (index)
          nodeData.value.arraySize = cast<ConstantInt>(index)->getZExtValue();

        auto result =
            m_resNodeDatas.insert(std::pair<ResourceNodeDataKey, ResourceMappingNodeType>(nodeData, nodeType));

        // Check if the node already had a different pair of node data/type. A DescriptorResource/DescriptorTexelBuffer
        // and a DescriptorSampler can use the same set/binding, in which case it is
        // DescriptorCombinedTexture.
        if (!result.second) {
          assert((nodeType == ResourceMappingNodeType::DescriptorCombinedTexture ||
                  nodeType == ResourceMappingNodeType::DescriptorResource ||
                  nodeType == ResourceMappingNodeType::DescriptorTexelBuffer ||
                  nodeType == ResourceMappingNodeType::DescriptorSampler) &&
                 (result.first->second == ResourceMappingNodeType::DescriptorCombinedTexture ||
                  result.first->second == ResourceMappingNodeType::DescriptorResource ||
                  result.first->second == ResourceMappingNodeType::DescriptorTexelBuffer ||
                  result.first->second == ResourceMappingNodeType::DescriptorSampler));
          result.first->second = ResourceMappingNodeType::DescriptorCombinedTexture;
        }
      }
    }
  }
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering operations for resource collecting.
INITIALIZE_PASS(SpirvLowerResourceCollect, DEBUG_TYPE, "Lower SPIR-V resource collecting", false, false)
