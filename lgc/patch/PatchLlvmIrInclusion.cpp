/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchLlvmIrInclusion.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchLlvmIrInclusion.
 ***********************************************************************************************************************
 */
#include "PatchLlvmIrInclusion.h"
#include "Abi.h"
#include "llvm/IR/Constants.h"

#define DEBUG_TYPE "llpc-patch-llvm-ir-inclusion"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchLlvmIrInclusion::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations of including LLVM IR as a separate section in the ELF.
ModulePass *createPatchLlvmIrInclusion() {
  return new PatchLlvmIrInclusion();
}

// =====================================================================================================================
PatchLlvmIrInclusion::PatchLlvmIrInclusion() : Patch(ID) {
}

// =====================================================================================================================
// Executes this patching pass on the specified LLVM module.
//
// This pass includes LLVM IR as a separate section in the ELF binary by inserting a new global variable with explicit
// section.
//
// @param [in,out] module : LLVM module to be run on
bool PatchLlvmIrInclusion::runOnModule(Module &module) {
  Patch::init(&module);

  std::string moduleStr;
  raw_string_ostream llvmIr(moduleStr);
  llvmIr << *m_module;
  llvmIr.flush();

  auto globalTy = ArrayType::get(Type::getInt8Ty(*m_context), moduleStr.size());
  auto initializer = ConstantDataArray::getString(m_module->getContext(), moduleStr, false);
  auto global = new GlobalVariable(*m_module, globalTy, true, GlobalValue::ExternalLinkage, initializer, "llvmir",
                                   nullptr, GlobalValue::NotThreadLocal, false);
  assert(global);

  std::string namePrefix = Util::Abi::AmdGpuCommentName;
  global->setSection(namePrefix + "llvmir");

  return true;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations of including LLVM IR as a separate section in the ELF binary.
INITIALIZE_PASS(PatchLlvmIrInclusion, DEBUG_TYPE, "Include LLVM IR as a separate section in the ELF binary", false,
                false)
