/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- DxilToLlvm.h -  --------------------------------------------------------------------------------------------===//
//
// This pass converts a DXIL module into an LLVM module by fixing constructs that have different semantics in the two.
// The output module will still contain DXIL intrinsics and metadata, because we only fix incompatibilities, and don't
// lower away DXIL.
//
// The following modifications are made:
//
// * i1 vectors are replaced by i32 vectors
//   This works around a more general difference between DXIL and LLVM:
//   In LLVM, vectors are always bit-packed and ignore the elements' alignment.
//   In DXIL, vectors respect the elements' alignment, and i1s have 32-bit alignment.
//   Thus, in DXIL, the elements of <2 x i1> are 32 bits apart, while they are bit-packed in LLVM,
//   and DXC relies on this by bit-casting allocas between <2 x i1> and <2 x i32>.
//   This only seems to affect HLSL i1 *matrices*, which are lowered to arrays of i1 vectors in DXIL,
//   and not HLSL i1 vectors, which are lowered to i32 arrays in DXIL.
//   To fix this, we replace all i1 vectors by i32 vectors.
//   We don't apply the same to other vectors that were overaligned in the original DXIL data layout (e.g. i16)
//   because this may harm performance, and we haven't observed cases yet where DXC relies on this layout.
//   See https://github.com/microsoft/DirectXShaderCompiler/issues/6082 for some background.
//
// Further known, not yet handled differences:
//
// * vectors of non-i1 elements that are overaligned in DXIL (see above)
// * potentially: overaligned types in general
//   After importing DXIL modules, we change the data layout to match what the backend does. Doing so potentially
//   breaks the module if it relies on the existing DL. For instance, after changing the alignment of i16 from 32 to 16,
//   storing as [4 x i16] and reading back the second dword behaves differently. Strictly speaking, when changing the
//   DL, we would need to update such occurrences. We don't do that because we haven't yet observed such cases, and
//   because it is difficult in general. For instance, we could transparently replace i16s by i32s to preserve the
//   32-bit size, but replacing half by float is more problematic. Although there is no spec, it appears DXC tries to
//   emit DXIL that supports such DL changes, by only using structured GEPs and avoiding transformations based on byte
//   offsets. Also, such fixups are only possible locally (e.g. for allocas), and not through opaque memory.
// * UDiv/URem/FPTrunc differences
// * fast math flags
//
//===--------------------------------------------------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace CompilerUtils {

class DxilToLlvmPass : public llvm::PassInfoMixin<DxilToLlvmPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Convert DXIL to LLVM IR"; }
};

} // namespace CompilerUtils
