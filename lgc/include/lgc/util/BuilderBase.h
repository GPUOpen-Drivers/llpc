/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderBase.h
 * @brief LLPC header file: declaration of BuilderBase
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/BuilderCommon.h"
#include "lgc/state/Defs.h"
#include "lgc/state/IntrinsDefs.h"

namespace lgc {

// =====================================================================================================================
// BuilderBase extends BuilderCommon, and provides some utility methods used within LGC.
// Methods here can be used directly from a BuilderImpl subclass, such as InOutBuilder.
// An LGC pass would have a BuilderCommon, and then use BuilderBase::get to turn it into a BuilderBase to
// access the methods here.
class BuilderBase : public BuilderCommon {
public:
  // Constructors
  BuilderBase(llvm::LLVMContext &context) : BuilderCommon(context) {}
  BuilderBase(llvm::BasicBlock *block) : BuilderCommon(block) {}
  BuilderBase(llvm::Instruction *inst) : BuilderCommon(inst) {}

  // Static method to use a BuilderCommon as a BuilderBase, relying on the fact that BuilderBase does not have
  // any additional state. This is needed when code in one of the builder implementation classes, such as
  // InOutBuilder, wants to use an LGC-internal method here in BuilderBase.
  static BuilderBase &get(BuilderCommon &builder) { return *static_cast<BuilderBase *>(&builder); }

  // Emits a amdgcn.reloc.constant intrinsic that represents an i32 relocatable value with the given symbol name
  //
  // @param symbolName : Name of the relocation symbol associated with this relocation
  llvm::Value *CreateRelocationConstant(const llvm::Twine &symbolName);

  // Generate an add of an offset to a byte pointer. This is provided to use in the case that the offset is,
  // or might be, a relocatable value, as it implements a workaround to get more efficient code for the load
  // that uses the offset pointer.
  //
  // @param pointer : Pointer to add to
  // @param byteOffset : Byte offset to add
  // @param instName : Name to give instruction
  llvm::Value *CreateAddByteOffset(llvm::Value *pointer, llvm::Value *byteOffset, const llvm::Twine &instName = "");

  // Type of function to pass to CreateMapToInt32
  typedef llvm::function_ref<llvm::Value *(BuilderBase &builder, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                           llvm::ArrayRef<llvm::Value *> passthroughArgs)>
      MapToInt32Func;

  // Create a call that'll map the massage arguments to an i32 type (for functions that only take i32).
  //
  // @param mapFunc : Pointer to the function to call on each i32.
  // @param mappedArgs : The arguments to massage into an i32 type.
  // @param passthroughArgs : The arguments to pass-through without massaging.
  llvm::Value *CreateMapToInt32(MapToInt32Func mapFunc, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                llvm::ArrayRef<llvm::Value *> passthroughArgs);

  // Create an inline assembly call to cause a side effect (used to work around miscompiles with convergent).
  llvm::Value *CreateInlineAsmSideEffect(llvm::Value *const value);

  // Create a call to set inactive. Both active and inactive should have the same type.
  llvm::Value *CreateSetInactive(llvm::Value *const active, llvm::Value *const inactive);

  // Create a subgroup broadcast.
  llvm::Value *CreateSubgroupBroadcastImpl(llvm::Value *const value, llvm::Value *const index,
                                           const llvm::Twine &instName = "");
  // Create a subgroup write invocation.
  llvm::Value *CreateSubgroupWriteInvocationImpl(llvm::Value *const inputValue, llvm::Value *const writeValue,
                                                 llvm::Value *const index, const llvm::Twine &instName = "");
  // Create a subgroup broadcast first.
  llvm::Value *CreateSubgroupBroadcastFirstImpl(llvm::Value *const value, const llvm::Twine &instName);
  // Create a call to dpp mov.
  llvm::Value *createDppMov(llvm::Value *const value, DppCtrl dppCtrl, unsigned rowMask, unsigned bankMask,
                            bool boundCtrl);
  // Create a call to permute lane.
  llvm::Value *createPermLane16(llvm::Value *const origValue, llvm::Value *const updateValue, unsigned selectBitsLow,
                                unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl);
  // Create a call to permute lane.
  llvm::Value *createPermLaneX16(llvm::Value *const origValue, llvm::Value *const updateValue, unsigned selectBitsLow,
                                 unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl);
  llvm::Value *createPermLane64(llvm::Value *const updateValue);
  // Create a call to ds swizzle.
  llvm::Value *createDsSwizzle(llvm::Value *const value, uint16_t dsPattern);
  // Create a ds_swizzle bit mode pattern.
  uint16_t getDsSwizzleBitMode(uint8_t xorMask, uint8_t orMask, uint8_t andMask);

  // Create a waterfall loop containing the specified instruction.
  llvm::Instruction *createWaterfallLoop(llvm::Instruction *nonUniformInst, llvm::ArrayRef<unsigned> operandIdxs,
                                         bool scalarizeDescriptorLoads = false, const llvm::Twine &instName = "");
};

} // namespace lgc
