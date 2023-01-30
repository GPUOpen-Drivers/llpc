/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchInitializeWorkgroupMemory.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchInitializeWorkgroupMemory.
 ***********************************************************************************************************************
 */

#include "lgc/patch/PatchInitializeWorkgroupMemory.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "lgc-patch-initialize-workgroup-memory"

using namespace lgc;
using namespace llvm;

static cl::opt<bool>
    ForceInitWorkgroupMemory("force-init-workgroup-memory",
                             cl::desc("Force to initialize the workgroup memory with zero for internal use"),
                             cl::init(false));
namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchInitializeWorkgroupMemory::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  if (runImpl(module, pipelineShaders, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchInitializeWorkgroupMemory::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                             PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Initialize-Workgroup-Memory\n");

  m_pipelineState = pipelineState;
  // This pass works on compute shader.
  if (!m_pipelineState->hasShaderStage(ShaderStageCompute))
    return false;

  SmallVector<GlobalVariable *> workgroupGlobals;
  for (GlobalVariable &global : module.globals()) {
    // The pass process the cases that the workgroup memory is forced to be initialized or the workgroup variable has an
    // zero initializer
    if (global.getType()->getPointerAddressSpace() == ADDR_SPACE_LOCAL &&
        (ForceInitWorkgroupMemory || (global.hasInitializer() && global.getInitializer()->isNullValue())))
      workgroupGlobals.push_back(&global);
  }

  if (workgroupGlobals.empty())
    return false;

  Patch::init(&module);
  m_shaderStage = ShaderStageCompute;
  m_entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(m_shaderStage));
  BuilderBase builder(*m_context);
  Instruction *insertPos = &*m_entryPoint->front().getFirstInsertionPt();
  builder.SetInsertPoint(insertPos);

  // Fill the map of each variable with zeroinitializer and calculate its corresponding offset on LDS
  unsigned offset = 0;
  for (auto global : workgroupGlobals) {
    unsigned varSize = getTypeSizeInDwords(global->getValueType());
    m_globalLdsOffsetMap.insert({global, builder.getInt32(offset)});
    offset += varSize;
  }

  // The new LDS is an i32 array
  const unsigned ldsSize = offset;
  auto ldsTy = ArrayType::get(builder.getInt32Ty(), ldsSize);
  auto lds = new GlobalVariable(module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, "lds", nullptr,
                                GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
  lds->setAlignment(MaybeAlign(16));

  // Replace the original LDS variables with the new LDS variable
  for (auto globalOffsetPair : m_globalLdsOffsetMap) {
    GlobalVariable *global = globalOffsetPair.first;
    Value *offset = globalOffsetPair.second;

    Value *pointer = builder.CreateGEP(lds->getValueType(), lds, {builder.getInt32(0), offset});
    pointer = builder.CreateBitCast(pointer, global->getType());

    global->replaceAllUsesWith(pointer);
    global->eraseFromParent();
  }

  initializeWithZero(lds, builder);

  return true;
}

// =====================================================================================================================
// Initialize the given LDS variable with zero.
//
// @param lds : The LDS variable to be initialized
// @param builder : BuilderBase to use for instruction constructing
void PatchInitializeWorkgroupMemory::initializeWithZero(GlobalVariable *lds, BuilderBase &builder) {
  auto entryInsertPos = &*m_entryPoint->front().getFirstInsertionPt();
  auto originBlock = entryInsertPos->getParent();
  auto endInitBlock = originBlock->splitBasicBlock(entryInsertPos);
  endInitBlock->setName(".endInit");

  auto initBlock = BasicBlock::Create(*m_context, ".init", originBlock->getParent(), endInitBlock);
  auto bodyBlock = BasicBlock::Create(*m_context, ".body", originBlock->getParent(), initBlock);
  auto forHeaderBlock = BasicBlock::Create(*m_context, ".for.header", originBlock->getParent(), bodyBlock);

  builder.SetInsertPoint(originBlock->getTerminator());
  // Get thread info
  auto &shaderMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
  Value *localInvocationId = getFunctionArgument(m_entryPoint, entryArgIdxs.cs.localInvocationId);
  const unsigned actualNumThreads = shaderMode.workgroupSizeX * shaderMode.workgroupSizeY * shaderMode.workgroupSizeZ;

  // On GFX11, it is a single VGPR and we need to extract the three components.
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) {
    assert(localInvocationId->getType() == builder.getInt32Ty());

    static constexpr unsigned LocalInvocationIdPackMask = 0x3FF;
    Value *unpackedLocalInvocationId = UndefValue::get(FixedVectorType::get(builder.getInt32Ty(), 3));

    // X = PackedId[9:0]
    unpackedLocalInvocationId = builder.CreateInsertElement(
        unpackedLocalInvocationId, builder.CreateAnd(localInvocationId, builder.getInt32(LocalInvocationIdPackMask)),
        uint64_t(0));

    // Y = PackedId[19:10]
    localInvocationId = builder.CreateLShr(localInvocationId, builder.getInt32(10));
    unpackedLocalInvocationId = builder.CreateInsertElement(
        unpackedLocalInvocationId, builder.CreateAnd(localInvocationId, builder.getInt32(LocalInvocationIdPackMask)),
        1);

    // Z = PackedId[29:20], PackedId[31:30] set to 0 by hardware
    localInvocationId = builder.CreateLShr(localInvocationId, builder.getInt32(10));
    unpackedLocalInvocationId = builder.CreateInsertElement(unpackedLocalInvocationId, localInvocationId, 2);

    localInvocationId = unpackedLocalInvocationId;
  }

  Value *threadId = builder.CreateExtractElement(localInvocationId, uint64_t(0));
  if (shaderMode.workgroupSizeY > 1) {
    Value *stride = builder.CreateMul(builder.getInt32(shaderMode.workgroupSizeX),
                                      builder.CreateExtractElement(localInvocationId, 1));
    threadId = builder.CreateAdd(threadId, stride);
  }
  if (shaderMode.workgroupSizeZ > 1) {
    Value *stride = builder.CreateMul(builder.getInt32(shaderMode.workgroupSizeX * shaderMode.workgroupSizeY),
                                      builder.CreateExtractElement(localInvocationId, 2));
    threadId = builder.CreateAdd(threadId, stride);
  }
  originBlock->getTerminator()->replaceUsesOfWith(endInitBlock, forHeaderBlock);

  // Each thread stores a zero to the continues LDS
  // for (int loopIdx = 0; loopIdx < loopCount; ++loopIdx) {
  //   if (threadId * loopCount + loopIdx < requiredNumThreads) {
  //      unsigned ldsOffset = (threadId * loopCount) + loopIdx;
  //      CreateStore(zero, ldsOffset);
  //   }
  //  }

  PHINode *loopIdxPhi = nullptr;
  const unsigned requiredNumThreads = lds->getValueType()->getArrayNumElements();
  Value *loopCount = builder.getInt32((requiredNumThreads + actualNumThreads - 1) / actualNumThreads);

  // Construct ".for.Header" block
  {
    builder.SetInsertPoint(forHeaderBlock);

    loopIdxPhi = builder.CreatePHI(builder.getInt32Ty(), 2);
    loopIdxPhi->addIncoming(builder.getInt32(0), originBlock);

    Value *isInLoop = builder.CreateICmpULT(loopIdxPhi, loopCount);
    builder.CreateCondBr(isInLoop, bodyBlock, endInitBlock);
  }

  // Construct ".body" block
  {
    builder.SetInsertPoint(bodyBlock);
    // The active thread is : threadId x loopCount + loopIdx < requiredNumThreads
    Value *index = builder.CreateMul(threadId, loopCount);
    index = builder.CreateAdd(index, loopIdxPhi);
    Value *isActiveThread = builder.CreateICmpULT(index, builder.getInt32(requiredNumThreads));
    builder.CreateCondBr(isActiveThread, initBlock, endInitBlock);

    // Construct ".init" block
    {
      builder.SetInsertPoint(initBlock);
      // ldsOffset = (threadId * loopCount) + loopIdx
      Value *ldsOffset = builder.CreateMul(threadId, loopCount);
      ldsOffset = builder.CreateAdd(ldsOffset, loopIdxPhi);
      Value *writePtr = builder.CreateGEP(lds->getValueType(), lds, {builder.getInt32(0), ldsOffset});
      builder.CreateAlignedStore(builder.getInt32(0), writePtr, Align(4));

      // Update loop index
      Value *loopNext = builder.CreateAdd(loopIdxPhi, builder.getInt32(1));
      loopIdxPhi->addIncoming(loopNext, initBlock);

      builder.CreateBr(forHeaderBlock);
    }
  }
  {
    // Set barrier after writing LDS
    builder.SetInsertPoint(&*endInitBlock->getFirstInsertionPt());
    SyncScope::ID workgroupScope = m_context->getOrInsertSyncScopeID("workgroup");
    builder.CreateFence(AtomicOrdering::Release, workgroupScope);
    builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    builder.CreateFence(AtomicOrdering::Acquire, workgroupScope);
  }
}

// =====================================================================================================================
// Return the size in dwords of a variable type
//
// @param inputTy : The type to be calculated
unsigned PatchInitializeWorkgroupMemory::getTypeSizeInDwords(Type *inputTy) {
  if (inputTy->isSingleValueType()) {
    // Variable in LDS is stored in dwords and padded as 4 dwords
    unsigned dwordCount = 4;
    unsigned elemCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
    if (inputTy->getScalarSizeInBits() == 64 && elemCount > 1)
      dwordCount = 8;
    return dwordCount;
  }
  if (inputTy->isArrayTy()) {
    const unsigned elemSize = getTypeSizeInDwords(inputTy->getArrayElementType());
    return inputTy->getArrayNumElements() * elemSize;
  }
  assert(inputTy->isStructTy());
  const unsigned memberCount = inputTy->getStructNumElements();
  unsigned memberSize = 0;
  for (unsigned idx = 0; idx < memberCount; ++idx)
    memberSize += getTypeSizeInDwords(inputTy->getStructElementType(idx));
  return memberSize;
}

} // namespace lgc
