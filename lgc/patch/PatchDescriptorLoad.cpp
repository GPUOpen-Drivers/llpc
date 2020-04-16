/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchDescriptorLoad.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchDescriptorLoad.
 ***********************************************************************************************************************
 */
#include "PatchDescriptorLoad.h"
#include "lgc/LgcContext.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-patch-descriptor-load"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchDescriptorLoad::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for descriptor load
ModulePass *createPatchDescriptorLoad() {
  return new PatchDescriptorLoad();
}

// =====================================================================================================================
PatchDescriptorLoad::PatchDescriptorLoad() : Patch(ID) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchDescriptorLoad::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Descriptor-Load\n");

  Patch::init(&module);
  m_changed = false;

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  m_pipelineSysValues.initialize(m_pipelineState);

  // Invoke handling of "call" instruction
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  for (unsigned shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage) {
    m_entryPoint = pipelineShaders->getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (m_entryPoint) {
      m_shaderStage = static_cast<ShaderStage>(shaderStage);
      visit(*m_entryPoint);
    }
  }

  // Remove unnecessary descriptor load calls
  for (auto callInst : m_descLoadCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_descLoadCalls.clear();

  // Remove unnecessary descriptor load functions
  for (auto func : m_descLoadFuncs) {
    if (func->user_empty()) {
      func->dropAllReferences();
      func->eraseFromParent();
    }
  }
  m_descLoadFuncs.clear();

  // Remove dead llpc.descriptor.point* and llpc.descriptor.index calls that were not
  // processed by the code above. That happens if they were never used in llpc.descriptor.load.from.ptr.
  SmallVector<Function *, 4> deadDescFuncs;
  for (Function &func : *m_module) {
    if (func.isDeclaration() && (func.getName().startswith(lgcName::DescriptorGetPtrPrefix) ||
                                 func.getName().startswith(lgcName::DescriptorIndex)))
      deadDescFuncs.push_back(&func);
  }
  for (Function *func : deadDescFuncs) {
    while (!func->use_empty())
      func->use_begin()->set(UndefValue::get(func->getType()));
    func->eraseFromParent();
  }

  m_pipelineSysValues.clear();
  return m_changed;
}

// =====================================================================================================================
// Process llpc.descriptor.get.{resource|sampler|fmask}.ptr call.
// This generates code to build a {ptr,stride} struct.
//
// @param descPtrCall : Call to llpc.descriptor.get.*.ptr
// @param descPtrCallName : Name of that call
void PatchDescriptorLoad::processDescriptorGetPtr(CallInst *descPtrCall, StringRef descPtrCallName) {
  m_entryPoint = descPtrCall->getFunction();
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(descPtrCall);

  // Find the resource node for the descriptor set and binding.
  unsigned descSet = cast<ConstantInt>(descPtrCall->getArgOperand(0))->getZExtValue();
  unsigned binding = cast<ConstantInt>(descPtrCall->getArgOperand(1))->getZExtValue();
  auto resType = ResourceNodeType::DescriptorResource;
  bool shadow = false;
  if (descPtrCallName.startswith(lgcName::DescriptorGetTexelBufferPtr))
    resType = ResourceNodeType::DescriptorTexelBuffer;
  else if (descPtrCallName.startswith(lgcName::DescriptorGetSamplerPtr))
    resType = ResourceNodeType::DescriptorSampler;
  else if (descPtrCallName.startswith(lgcName::DescriptorGetFmaskPtr)) {
    shadow = m_pipelineSysValues.get(m_entryPoint)->isShadowDescTableEnabled();
    resType = ResourceNodeType::DescriptorFmask;
  }

  // Find the descriptor node. For fmask with -enable-shadow-descriptor-table, if no fmask descriptor
  // is found, look for a resource (image) one instead.
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  std::tie(topNode, node) = m_pipelineState->findResourceNode(resType, descSet, binding);
  if (!node && resType == ResourceNodeType::DescriptorFmask && shadow) {
    std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorResource, descSet, binding);
  }

  Value *descPtrAndStride = nullptr;
  if (!node) {
    // We did not find the resource node. Use an undef value.
    descPtrAndStride = UndefValue::get(descPtrCall->getType());
  } else {
    // Get the descriptor pointer and stride as a struct.
    descPtrAndStride = getDescPtrAndStride(resType, descSet, binding, topNode, node, shadow, builder);
  }
  descPtrCall->replaceAllUsesWith(descPtrAndStride);
  m_descLoadCalls.push_back(descPtrCall);
  m_changed = true;
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
// @param [in/out] builder : IRBuilder
Value *PatchDescriptorLoad::getDescPtrAndStride(ResourceNodeType resType, unsigned descSet, unsigned binding,
                                                const ResourceNode *topNode, const ResourceNode *node, bool shadow,
                                                BuilderBase &builder) {
  // Determine the stride if possible without looking at the resource node.
  unsigned byteSize = 0;
  Value *stride = nullptr;
  switch (resType) {
  case ResourceNodeType::DescriptorBuffer:
  case ResourceNodeType::DescriptorTexelBuffer:
    byteSize = DescriptorSizeBuffer;
    if (node && node->type == ResourceNodeType::DescriptorBufferCompact)
      byteSize = DescriptorSizeBufferCompact;
    stride = builder.getInt32(byteSize);
    break;
  case ResourceNodeType::DescriptorSampler:
    byteSize = DescriptorSizeSampler;
    break;
  case ResourceNodeType::DescriptorResource:
  case ResourceNodeType::DescriptorFmask:
    byteSize = DescriptorSizeResource;
    break;
  default:
    llvm_unreachable("");
    break;
  }

  if (!stride) {
    // Stride is not determinable just from the descriptor type requested by the Builder call.
    if (m_pipelineState->isUnlinked()) {
      // Shader compilation: Get byte stride using a reloc.
      stride = builder.CreateRelocationConstant("dstride_" + Twine(descSet) + "_" + Twine(binding));
    } else {
      // Pipeline compilation: Get the stride from the resource type in the node.
      assert(node && "expected valid user data node to determine descriptor stride.");
      switch (node->type) {
      case ResourceNodeType::DescriptorSampler:
        stride = builder.getInt32(DescriptorSizeSampler);
        break;
      case ResourceNodeType::DescriptorResource:
      case ResourceNodeType::DescriptorFmask:
        stride = builder.getInt32(DescriptorSizeResource);
        break;
      case ResourceNodeType::DescriptorCombinedTexture:
        stride = builder.getInt32(DescriptorSizeResource + DescriptorSizeSampler);
        break;
      case ResourceNodeType::DescriptorYCbCrSampler:
        stride = builder.getInt32(0);
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
    std::string globalName = (Twine("_immutable_sampler_") + Twine(node->set) + " " + Twine(node->binding)).str();
    descPtr = m_module->getGlobalVariable(globalName, /*AllowInternal=*/true);
    if (!descPtr) {
      descPtr = new GlobalVariable(*m_module, node->immutableValue->getType(),
                                   /*isConstant=*/true, GlobalValue::InternalLinkage, node->immutableValue, globalName,
                                   nullptr, GlobalValue::NotThreadLocal, ADDR_SPACE_CONST);
    }
    descPtr = builder.CreateBitCast(descPtr, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));

    // We need to change the stride to 4 dwords. It would otherwise be incorrectly set to 12 dwords
    // for a sampler in a combined texture.
    stride = builder.getInt32(node->type == ResourceNodeType::DescriptorYCbCrSampler ? DescriptorSizeSamplerYCbCr
                                                                                     : DescriptorSizeSampler);

  } else {
    // Get a pointer to the descriptor.
    descPtr = getDescPtr(resType, descSet, binding, topNode, node, shadow, builder);
  }

  // Cast the pointer to the right type and create and return the struct.
  descPtr = builder.CreateBitCast(descPtr,
                                  VectorType::get(builder.getInt32Ty(), byteSize / 4)->getPointerTo(ADDR_SPACE_CONST));
  Value *descPtrStruct = builder.CreateInsertValue(
      UndefValue::get(StructType::get(*m_context, {descPtr->getType(), builder.getInt32Ty()})), descPtr, 0);
  descPtrStruct = builder.CreateInsertValue(descPtrStruct, stride, 1);
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
// @param [in/out] builder : IRBuilder
Value *PatchDescriptorLoad::getDescPtr(ResourceNodeType resType, unsigned descSet, unsigned binding,
                                       const ResourceNode *topNode, const ResourceNode *node, bool shadow,
                                       BuilderBase &builder) {
  Value *descPtr = nullptr;
  // Get the descriptor table pointer.
  auto sysValues = m_pipelineSysValues.get(builder.GetInsertPoint()->getFunction());
  if (node && node == topNode) {
    // The descriptor is in the top-level table. Contrary to what used to happen, we just load from
    // the spill table, so we can get a pointer to the descriptor. It gets returned as a pointer
    // to array of i8.
    descPtr = sysValues->getSpillTablePtr();
  } else if (shadow) {
    // Get pointer to descriptor set's descriptor table as pointer to i8.
    descPtr = sysValues->getShadowDescTablePtr(descSet);
  } else {
    // Get pointer to descriptor set's descriptor table. This also gets returned as a pointer to
    // array of i8.
    descPtr = sysValues->getDescTablePtr(descSet);
  }

  // Add on the byte offset of the descriptor.
  Value *offset = nullptr;
  bool useRelocationForOffsets = !node || m_pipelineState->isUnlinked();
  if (useRelocationForOffsets) {
    // Get the offset for the descriptor using a reloc. The reloc symbol name
    // needs to contain the descriptor set and binding, and, for image, fmask or sampler, whether it is
    // a sampler.
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
    offset = builder.CreateRelocationConstant("doff_" + Twine(descSet) + "_" + Twine(binding) + relocNameSuffix);
    // The LLVM's internal handling of GEP instruction results in a lot of junk code and prevented selection
    // of the offset-from-register variant of the s_load_dwordx4 instruction. To workaround this issue,
    // we use integer arithmetic here so the amdgpu backend can pickup the optimal instruction.
    // When relocation is used, offset is in bytes, not in dwords.
    descPtr = builder.CreatePtrToInt(descPtr, builder.getInt64Ty());
    descPtr = builder.CreateAdd(descPtr, builder.CreateZExt(offset, builder.getInt64Ty()));
    descPtr = builder.CreateIntToPtr(descPtr, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));
  } else {
    // Get the offset for the descriptor. Where we are getting the second part of a combined resource,
    // add on the size of the first part.
    unsigned offsetInBytes = node->offsetInDwords * 4;
    if (resType == ResourceNodeType::DescriptorSampler && (node->type == ResourceNodeType::DescriptorCombinedTexture ||
                                                           node->type == ResourceNodeType::DescriptorYCbCrSampler))
      offsetInBytes += DescriptorSizeResource;
    offset = builder.getInt32(offsetInBytes);
    descPtr = builder.CreateBitCast(descPtr, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));
    descPtr = builder.CreateGEP(builder.getInt8Ty(), descPtr, offset);
  }
  return descPtr;
}

// =====================================================================================================================
// Process an llvm.descriptor.index : add an array index on to the descriptor pointer.
// llvm.descriptor.index has two operands: the "descriptor pointer" (actually a struct containing the actual
// pointer and an int giving the byte stride), and the index to add. It returns the updated "descriptor pointer".
//
// @param call : llvm.descriptor.index call
void PatchDescriptorLoad::processDescriptorIndex(CallInst *call) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(call);

  Value *descPtrStruct = call->getArgOperand(0);
  Value *index = call->getArgOperand(1);
  Value *stride = builder.CreateExtractValue(descPtrStruct, 1);
  Value *descPtr = builder.CreateExtractValue(descPtrStruct, 0);

  Value *bytePtr = builder.CreateBitCast(descPtr, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));
  index = builder.CreateMul(index, stride);
  bytePtr = builder.CreateGEP(builder.getInt8Ty(), bytePtr, index);
  descPtr = builder.CreateBitCast(bytePtr, descPtr->getType());

  descPtrStruct = builder.CreateInsertValue(
      UndefValue::get(StructType::get(*m_context, {descPtr->getType(), builder.getInt32Ty()})), descPtr, 0);
  descPtrStruct = builder.CreateInsertValue(descPtrStruct, stride, 1);

  call->replaceAllUsesWith(descPtrStruct);
  m_descLoadCalls.push_back(call);
  m_changed = true;
}

// =====================================================================================================================
// Process "llpc.descriptor.load.from.ptr" call.
//
// @param loadFromPtr : Call to llpc.descriptor.load.from.ptr
void PatchDescriptorLoad::processLoadDescFromPtr(CallInst *loadFromPtr) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(loadFromPtr);

  Value *descPtrStruct = loadFromPtr->getArgOperand(0);
  Value *descPtr = builder.CreateExtractValue(descPtrStruct, 0);
  Value *desc = builder.CreateLoad(loadFromPtr->getType(), descPtr);

  loadFromPtr->replaceAllUsesWith(desc);
  m_descLoadCalls.push_back(loadFromPtr);
  m_changed = true;
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchDescriptorLoad::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  StringRef mangledName = callee->getName();

  if (mangledName.startswith(lgcName::DescriptorGetPtrPrefix)) {
    processDescriptorGetPtr(&callInst, mangledName);
    return;
  }

  if (mangledName.startswith(lgcName::DescriptorIndex)) {
    processDescriptorIndex(&callInst);
    return;
  }

  if (mangledName.startswith(lgcName::DescriptorLoadFromPtr)) {
    processLoadDescFromPtr(&callInst);
    return;
  }

  if (mangledName.startswith(lgcName::DescriptorLoadSpillTable)) {
    // Descriptor loading should be inlined and stay in shader entry-point
    assert(callInst.getParent()->getParent() == m_entryPoint);
    m_changed = true;

    if (!callInst.use_empty()) {
      Value *desc = m_pipelineSysValues.get(m_entryPoint)->getSpilledPushConstTablePtr();
      if (desc->getType() != callInst.getType())
        desc = new BitCastInst(desc, callInst.getType(), "", &callInst);
      callInst.replaceAllUsesWith(desc);
    }
    m_descLoadCalls.push_back(&callInst);
    m_descLoadFuncs.insert(callee);
    return;
  }

  if (mangledName.startswith(lgcName::DescriptorLoadBuffer)) {
    // Descriptor loading should be inlined and stay in shader entry-point
    assert(callInst.getParent()->getParent() == m_entryPoint);
    m_changed = true;

    if (!callInst.use_empty()) {
      unsigned descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
      unsigned binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
      Value *arrayOffset = callInst.getOperand(2); // Offset for arrayed resource (index)
      Value *desc = loadBufferDescriptor(descSet, binding, arrayOffset, &callInst);
      callInst.replaceAllUsesWith(desc);
    }
    m_descLoadCalls.push_back(&callInst);
    m_descLoadFuncs.insert(callee);
    return;
  }
}

// =====================================================================================================================
// Generate the code for a buffer descriptor load.
// This is the handler for llpc.descriptor.load.buffer, which is also used for loading a descriptor
// from the global table or the per-shader table.
//
// @param descSet : Descriptor set
// @param binding : Binding
// @param arrayOffset : Index in descriptor array
// @param insertPoint : Insert point
Value *PatchDescriptorLoad::loadBufferDescriptor(unsigned descSet, unsigned binding, Value *arrayOffset,
                                                 Instruction *insertPoint) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPoint);

  // Handle the special cases. First get a pointer to the global/per-shader table as pointer to i8.
  Value *descPtr = nullptr;
  if (descSet == InternalResourceTable)
    descPtr = m_pipelineSysValues.get(m_entryPoint)->getInternalGlobalTablePtr();
  else if (descSet == InternalPerShaderTable)
    descPtr = m_pipelineSysValues.get(m_entryPoint)->getInternalPerShaderTablePtr();
  if (descPtr) {
    // "binding" gives the offset, in units of v4i32 descriptors.
    // Add on the offset, giving pointer to i8.
    descPtr = builder.CreateGEP(builder.getInt8Ty(), descPtr, builder.getInt32(binding * DescriptorSizeBuffer));
    // Load the descriptor.
    Type *descTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBuffer / 4);
    descPtr = builder.CreateBitCast(descPtr, descTy->getPointerTo(ADDR_SPACE_CONST));
    Instruction *load = builder.CreateLoad(descTy, descPtr);
    load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(load->getContext(), {}));
    return load;
  }

  // Normal buffer descriptor load.
  // Find the descriptor node, either a DescriptorBuffer or PushConst (inline buffer).
  const ResourceNode *topNode = nullptr;
  const ResourceNode *node = nullptr;
  std::tie(topNode, node) = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorBuffer, descSet, binding);

  if (!node) {
    // We did not find the resource node. Use an undef value.
    return UndefValue::get(VectorType::get(builder.getInt32Ty(), 4));
  }

  if (node == topNode && node->type == ResourceNodeType::DescriptorBufferCompact) {
    // This is a compact buffer descriptor (only two dwords) in the top-level table. We special-case
    // that to use user data SGPRs directly, if PatchEntryPointMutate managed to fit the value into
    // user data SGPRs.
    unsigned resNodeIdx = topNode - m_pipelineState->getUserDataNodes().data();
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    unsigned argIdx = intfData->entryArgIdxs.resNodeValues[resNodeIdx];
    if (argIdx > 0) {
      // Resource node isn't spilled. Load its value from function argument.
      Argument *descArg = m_entryPoint->getArg(argIdx);
      descArg->setName(Twine("resNode") + Twine(resNodeIdx));
      // The function argument is a vector of i32. Treat it as an array of <2 x i32> and
      // extract the required array element.
      arrayOffset = builder.CreateMul(arrayOffset, builder.getInt32(2));
      Value *descDword0 = builder.CreateExtractElement(descArg, arrayOffset);
      arrayOffset = builder.CreateAdd(arrayOffset, builder.getInt32(1));
      Value *descDword1 = builder.CreateExtractElement(descArg, arrayOffset);
      Value *desc = UndefValue::get(VectorType::get(builder.getInt32Ty(), 2));
      desc = builder.CreateInsertElement(desc, descDword0, uint64_t(0));
      desc = builder.CreateInsertElement(desc, descDword1, 1);
      return buildBufferCompactDesc(desc, &*builder.GetInsertPoint());
    }
  }

  // Get a pointer to the descriptor, as a pointer to 8
  descPtr = getDescPtr(ResourceNodeType::DescriptorBuffer, descSet, binding, topNode, node,
                       /*shadow=*/false, builder);

  if (node && node->type == ResourceNodeType::PushConst) {
    // Inline buffer.
    return buildInlineBufferDesc(descPtr, builder);
  }

  // Add on the index.
  unsigned byteStride = DescriptorSizeBuffer;
  if (node && node->type == ResourceNodeType::DescriptorBufferCompact)
    byteStride = DescriptorSizeBufferCompact;
  arrayOffset = builder.CreateMul(arrayOffset, builder.getInt32(byteStride));
  descPtr = builder.CreateGEP(builder.getInt8Ty(), descPtr, arrayOffset);

  if (byteStride == DescriptorSizeBufferCompact) {
    // Load compact buffer descriptor and convert it into a normal buffer descriptor.
    Type *descTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBufferCompact / 4);
    descPtr = builder.CreateBitCast(descPtr, descTy->getPointerTo(ADDR_SPACE_CONST));
    Value *desc = builder.CreateLoad(descTy, descPtr);
    return buildBufferCompactDesc(desc, &*builder.GetInsertPoint());
  }

  // Load normal buffer descriptor.
  Type *descTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBuffer / 4);
  descPtr = builder.CreateBitCast(descPtr, descTy->getPointerTo(ADDR_SPACE_CONST));
  Instruction *load = builder.CreateLoad(descTy, descPtr);
  load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(load->getContext(), {}));
  return load;
}

// =====================================================================================================================
// Calculate a buffer descriptor for an inline buffer
//
// @param descPtr : Pointer to inline buffer
// @param builder : Builder
Value *PatchDescriptorLoad::buildInlineBufferDesc(Value *descPtr, IRBuilder<> &builder) {
  // Bitcast the pointer to v2i32
  descPtr = builder.CreatePtrToInt(descPtr, builder.getInt64Ty());
  descPtr = builder.CreateBitCast(descPtr, VectorType::get(builder.getInt32Ty(), 2));

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

  // DWORD0
  Value *desc = UndefValue::get(VectorType::get(builder.getInt32Ty(), 4));
  Value *descElem0 = builder.CreateExtractElement(descPtr, uint64_t(0));
  desc = builder.CreateInsertElement(desc, descElem0, uint64_t(0));

  // DWORD1
  Value *descElem1 = builder.CreateExtractElement(descPtr, 1);
  descElem1 = builder.CreateAnd(descElem1, builder.getInt32(sqBufRsrcWord1.u32All));
  desc = builder.CreateInsertElement(desc, descElem1, 1);

  // DWORD2
  desc = builder.CreateInsertElement(desc, builder.getInt32(sqBufRsrcWord2.u32All), 2);

  // DWORD3
  desc = builder.CreateInsertElement(desc, builder.getInt32(sqBufRsrcWord3.u32All), 3);

  return desc;
}

// =====================================================================================================================
// Build buffer compact descriptor
//
// @param desc : The buffer descriptor base to build for the buffer compact descriptor
// @param insertPoint : Insert point
Value *PatchDescriptorLoad::buildBufferCompactDesc(Value *desc, Instruction *insertPoint) {
  // Extract compact buffer descriptor
  Value *descElem0 =
      ExtractElementInst::Create(desc, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPoint);

  Value *descElem1 =
      ExtractElementInst::Create(desc, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPoint);

  // Build normal buffer descriptor
  auto bufDescTy = VectorType::get(Type::getInt32Ty(*m_context), 4);
  Value *bufDesc = UndefValue::get(bufDescTy);

  // DWORD0
  bufDesc =
      InsertElementInst::Create(bufDesc, descElem0, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPoint);

  // DWORD1
  SqBufRsrcWord1 sqBufRsrcWord1 = {};
  sqBufRsrcWord1.bits.baseAddressHi = UINT16_MAX;

  descElem1 = BinaryOperator::CreateAnd(
      descElem1, ConstantInt::get(Type::getInt32Ty(*m_context), sqBufRsrcWord1.u32All), "", insertPoint);

  bufDesc =
      InsertElementInst::Create(bufDesc, descElem1, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPoint);

  // DWORD2
  SqBufRsrcWord2 sqBufRsrcWord2 = {};
  sqBufRsrcWord2.bits.numRecords = UINT32_MAX;

  bufDesc = InsertElementInst::Create(bufDesc, ConstantInt::get(Type::getInt32Ty(*m_context), sqBufRsrcWord2.u32All),
                                      ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPoint);

  // DWORD3
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

    bufDesc = InsertElementInst::Create(bufDesc, ConstantInt::get(Type::getInt32Ty(*m_context), sqBufRsrcWord3.u32All),
                                        ConstantInt::get(Type::getInt32Ty(*m_context), 3), "", insertPoint);
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

    bufDesc = InsertElementInst::Create(bufDesc, ConstantInt::get(Type::getInt32Ty(*m_context), sqBufRsrcWord3.u32All),
                                        ConstantInt::get(Type::getInt32Ty(*m_context), 3), "", insertPoint);
  } else
    llvm_unreachable("Not implemented!");

  return bufDesc;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for descriptor load.
INITIALIZE_PASS(PatchDescriptorLoad, DEBUG_TYPE, "Patch LLVM for descriptor load operations", false, false)
