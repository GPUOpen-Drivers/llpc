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
 * @file  Debug.cpp
 * @brief LLPC header file: middle-end debug functions
 ***********************************************************************************************************************
 */
#include "lgc/Debug.h"
#include "lgc/LgcContext.h"

using namespace llvm;

namespace lgc {

//======================================================================================================================
// Get pointer to stream for LLPC_OUTS, or nullptr if disabled.
raw_ostream *getLgcOuts() {
  return LgcContext::getLgcOuts();
}

void InstructionSlot::createFuncSlot(Function *func) {
  m_iMap.clear();
  m_valueIndex = 0;
  for (Argument &arg : func->args())
    if (!arg.hasName())
      createSlot(&arg);

  // Add all of the basic blocks and instructions with no names.
  for (auto &bb : *func) {
    if (!bb.hasName())
      createSlot(&bb);

    for (auto &inst : bb) {
      if (!inst.getType()->isVoidTy())
        createSlot(&inst);
    }
  }
}

Value *InstructionSlot::getValueByIdx(unsigned idx) {
  if (m_iMap.find(idx) != m_iMap.end())
    return m_iMap[idx];
  return nullptr;
}

Value *InstructionSlot::getValueByName(StringRef name) {
  if (m_nMap.find(name) != m_nMap.end())
    return m_nMap[name];
  return nullptr;
}

void InstructionSlot::createSlot(Value *val) {
  if (val->hasName())
    m_nMap[val->getName()] = val;
  else {
    unsigned destSlot = m_valueIndex++;
    m_iMap[destSlot] = val;
  }
}
} // namespace lgc
