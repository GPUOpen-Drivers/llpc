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

include "llvm-dialects/Dialect/Dialect.td"

def LlpcDialect : Dialect {
  let name = "llpc";
  let cppNamespace = "Llpc";
}

def ConstantPointer : TgConstant<(PointerType 4)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;
def BufferPointer : TgConstant<(PointerType 7)>, Type;

class LlpcOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LlpcDialect, mnemonic_, traits_ # [NoUnwind]>;

def StructuralGepOp : LlpcOp<"structural.gep", [Memory<[]>, WillReturn]> {
  let arguments = (ins PointerType:$base_pointer, type:$base_type, AttrI1:$inbound, varargs:$indices);
  let results = (outs (eq $base_pointer):$result);

  let summary = "GEP a pointer from a base object pointer";
  let description = [{
    Returns a opaque in/out pointer from a base object pointer through structural gep of indices.

    'base_pointer' is the opaque pointer of base.
    'type' is the type of in/out data type.
    'inbound' is the bool attribute mark if access chain is in bounds.
    'indices' is indices from spir-v.
  }];
}
