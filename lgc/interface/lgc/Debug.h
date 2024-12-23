/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  Debug.h
 * @brief LLPC header file: declaration of LLPC_OUTS for use in the middle-end
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

namespace lgc {

// Get pointer to stream for LLPC_OUTS, or nullptr if disabled.
llvm::raw_ostream *getLgcOuts();

// Create Indexed instruction slots for all the instruction of the function, the instruction in the function
// can later be referenced by index
// Usage:
//   1) use amdllpc/llpc -print-after-all to get the module pass dump, instructions names/indices
//   2) At the end of the pass processing place, you can insert instructions

//     for (Function &decl : module) {
//         if (decl.getName().ends_with("RayGen@@YAXXZ")) {
//             lgc::Builder builder(module.getContext());
//             InstructionSlot instSlot;
//             instSlot.createFuncSlot(&decl);
//             auto bufferDesc = instSlot.getValueByIdx(46);
//             auto nextPos = (cast<Instruction>(bufferDesc))->getNextNode();
//             builder.SetInsertPoint(nextPos);
//             SmallVector<Value *> lists = {bufferDesc};
//             builder.create<lgc::DebugPrintfOp>("desc:%d\n", lists);
//        }
//     }

class InstructionSlot {
  unsigned m_valueIndex = 0;
  using IndexMap = llvm::DenseMap<unsigned, llvm::Value *>;
  using NamedMap = llvm::DenseMap<llvm::StringRef, llvm::Value *>;
  IndexMap m_iMap;
  NamedMap m_nMap;

  void createSlot(llvm::Value *val);

public:
  InstructionSlot(llvm::Function *func) { createFuncSlot(func); }
  void createFuncSlot(llvm::Function *func);
  llvm::Value *getValueByIdx(unsigned idx);
  llvm::Value *getValueByName(llvm::StringRef name);
};
} // namespace lgc

// Output general message
#define LLPC_OUTS(msg)                                                                                                 \
  do                                                                                                                   \
    if (llvm::raw_ostream *pStream = getLgcOuts()) {                                                                   \
      *pStream << msg;                                                                                                 \
    }                                                                                                                  \
  while (false)
