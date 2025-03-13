/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

include "llvm-dialects/Dialect/Dialect.td"

def LgcXdlDialect : Dialect {
  let name = "lgc.xdl";
  let cppNamespace = "lgc::xdl";
}

def ConstantPointer : TgConstant<(PointerType 4)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;
def BufferPointer : TgConstant<(PointerType 7)>, Type;
def BufferStridedPointer : TgConstant<(PointerType 9)>, Type;
def TaskPayloadPointer : TgConstant<(PointerType 7)>, Type;

#ifdef LLVM_HAVE_NODIVERGENCESOURCE_ATTR
def NoDivergenceSource : LlvmEnumAttributeTrait<"NoDivergenceSource">;
#endif

defm CooperativeMatrixMemoryAccess : AttrEnum<"CooperativeMatrixMemoryAccess">;
defm CooperativeMatrixElementType : AttrEnum<"CooperativeMatrixElementType">;
defm CooperativeMatrixLayout : AttrEnum<"CooperativeMatrixLayout">;
defm CooperativeMatrixArithOp : AttrEnum<"CooperativeMatrixArithOp">;

class DivergentLgcXdlOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LgcXdlDialect, mnemonic_, traits_ # [NoUnwind]>;

class LgcXdlOp<string mnemonic_, list<Trait> traits_ = []>
    : DivergentLgcXdlOp<mnemonic_, traits_
#ifdef LLVM_HAVE_NODIVERGENCESOURCE_ATTR
                                           # [NoDivergenceSource]
#endif
                        >;

class LgcXdlIntrinOp<string mnemonic_, list<Trait> traits_ = []>
    : LgcXdlOp<!strconcat("intrin.", mnemonic_), traits_>;

include "lgc/CooperativeMatrix.td"
include "lgc/RowAccumulator.td"
#ifdef LLPC_BUILD_GFX12
include "lgc/Sparse.td"
#endif
