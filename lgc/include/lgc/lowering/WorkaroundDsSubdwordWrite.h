/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  WorkaroundDsSubdwordWrite.h
 * @brief LLPC header file: contains declaration of class lgc::WorkaroundDsSubdwordWrite.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/util/Internal.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass applying a sub-dword DS store workaround:
//
// -  There is a bug (planned to be fixed) on gfx1150 with sub-dword writes
//    to LDS. All sub-dword DS write ops are broken in the scenario when more
//    than 1 thread of a wave32 has the same dword address, but different sub-dword
//    address. Work around the issue by placing a waterfall loop
//    around the ds_write, ensuring that the address written to is the same in
//    all lanes.
//
//
class WorkaroundDsSubdwordWrite final : public llvm::PassInfoMixin<WorkaroundDsSubdwordWrite> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Workaround DS sub-dword write (GFX1150)"; }
};

} // namespace lgc
