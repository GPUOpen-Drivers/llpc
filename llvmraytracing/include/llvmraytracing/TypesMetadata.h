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
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
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

//===- TypesMetadata.h - Pointee type metadata for processing DXIL ---------==//

#pragma once

#include "llvm/Bitcode/BitcodeReader.h"

namespace llvm {

/// Return element type of a function argument resolving opaque pointers
/// via !types metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Argument *Arg);

/// Return element type of a function argument resolving opaque pointers
/// via !types metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Function *F, int ArgNo);

/// LLVM parser callback which adds !types metadata during DXIL parsing
void DXILValueTypeMetadataCallback(Value *V, unsigned TypeID,
                                   GetTypeByIDTy GetTypeByID,
                                   GetContainedTypeIDTy GetContainedTypeID);

} // namespace llvm
