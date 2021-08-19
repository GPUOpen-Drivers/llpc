/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "lgc/BuilderBase.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"

#define DEBUG_TYPE "lgc-patch-initialize-workgroup-memory"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Represents the pass of setting up the value for workgroup global variables.
class PatchInitializeWorkgroupMemory final : public Patch {
public:
  PatchInitializeWorkgroupMemory();

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

  virtual bool runOnModule(Module &module) override;

  static char ID; // ID of this pass

private:
  PatchInitializeWorkgroupMemory(const PatchInitializeWorkgroupMemory &) = delete;
  PatchInitializeWorkgroupMemory &operator=(const PatchInitializeWorkgroupMemory &) = delete;

  void initializeWithZero(Value *pointer, Type *valueTy, SmallVectorImpl<Value *> &indices, BuilderBase &builder);
};

// =====================================================================================================================
// Initializes static members.
char PatchInitializeWorkgroupMemory::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of setting up the value for workgroup global variables.
ModulePass *createPatchInitializeWorkgroupMemory() {
  return new PatchInitializeWorkgroupMemory();
}

// =====================================================================================================================
PatchInitializeWorkgroupMemory::PatchInitializeWorkgroupMemory() : Patch(ID) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchInitializeWorkgroupMemory::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Initialize-Workgroup-Memory\n");

  PipelineState *pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  // This pass works on compute shader.
  if (!pipelineState->hasShaderStage(ShaderStageCompute))
    return false;

  SmallVector<GlobalVariable *> zeroGlobals;
  for (GlobalVariable &global : module.globals()) {
    if (global.hasInitializer() && global.getInitializer()->isNullValue())
      zeroGlobals.push_back(&global);
  }

  if (zeroGlobals.empty())
    return false;

  Patch::init(&module);
  m_shaderStage = ShaderStageCompute;
  m_entryPoint = (&getAnalysis<PipelineShaders>())->getEntryPoint(static_cast<ShaderStage>(m_shaderStage));
  BuilderBase builder(*m_context);
  Instruction *insertPos = &*m_entryPoint->front().getFirstInsertionPt();
  builder.SetInsertPoint(insertPos);

  for (auto global : zeroGlobals) {
    Type *globalTy = global->getType();
    // Create new global with null initializer to replace the original global. The global with zeroinitializer is not
    // supported by back-end
    GlobalVariable *newGlobal =
        new GlobalVariable(module, globalTy->getPointerElementType(), false, GlobalValue::ExternalLinkage, nullptr,
                           "lds", nullptr, GlobalValue::NotThreadLocal, globalTy->getPointerAddressSpace());
    newGlobal->setAlignment(MaybeAlign(global->getAlignment()));
    global->replaceAllUsesWith(newGlobal);

    Type *ptrElemTy = globalTy->getPointerElementType();
    SmallVector<Value *> indices;
    if (ptrElemTy->isAggregateType())
      indices.push_back(builder.getInt32(0));
    initializeWithZero(newGlobal, ptrElemTy, indices, builder);

    global->eraseFromParent();
  }

  return true;
}

// =====================================================================================================================
// Recursively search the single value for a given type and write the corresponding zero value to the specified address
// on LDS.

//
// @param pointer : The pointer to the address on LDS
// @param valueTy : The checking value type
// @param indices : The index list required for an aggregative type
// @param builder : BuilderBase to use for instruction constructing
void PatchInitializeWorkgroupMemory::initializeWithZero(Value *pointer, Type *valueTy,
                                                        SmallVectorImpl<Value *> &indices, BuilderBase &builder) {
  if (valueTy->isSingleValueType()) {
    Value *zero = Constant::getNullValue(valueTy);
    unsigned alignment = valueTy->getPrimitiveSizeInBits() / 8;
    if (!isPowerOf2_32(alignment))
      alignment = NextPowerOf2(alignment);
    if (!indices.empty())
      pointer = builder.CreateGEP(pointer, indices);
    builder.CreateAlignedStore(zero, pointer, Align(alignment));
    return;
  } else if (valueTy->isArrayTy()) {
    const unsigned elemCount = valueTy->getArrayNumElements();
    valueTy = valueTy->getContainedType(0);
    for (unsigned idx = 0; idx < elemCount; ++idx) {
      indices.push_back(builder.getInt32(idx));
      initializeWithZero(pointer, valueTy, indices, builder);
      indices.pop_back();
    }
  } else {
    assert(valueTy->isStructTy());
    const unsigned memberCount = valueTy->getStructNumElements();
    for (unsigned idx = 0; idx < memberCount; ++idx) {
      Type *memberTy = valueTy->getStructElementType(idx);
      indices.push_back(builder.getInt32(idx));
      initializeWithZero(pointer, memberTy, indices, builder);
      indices.pop_back();
    }
  }
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of initialize workgroup memory with zero.
INITIALIZE_PASS(PatchInitializeWorkgroupMemory, DEBUG_TYPE, "Patch for initialize workgroup memory", false, false)
