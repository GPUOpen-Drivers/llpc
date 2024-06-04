/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- LgcIlCpsDialect.td - Dialect definitions -------*- tablegen -*-===//

include "llvm-dialects/Dialect/Dialect.td"

def LgcIlCpsDialect : Dialect {
  let name = "lgc.ilcps";
  let cppNamespace = "lgc::ilcps";
}

class LgcIlCpsOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LgcIlCpsDialect, mnemonic_, traits_ # [NoUnwind]>;

def GetReturnValueOp : LgcIlCpsOp<"getReturnValue", [NoUnwind, WillReturn]> {
  let arguments = (ins);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary =
    "represents the return value of an asynchronously called function";
  let description = [{
    The coroutine passes do not support passing additional arguments to resume
    functions. We add this intrinsic after a resume point that creates
    a value out of thin air. After the coroutine passes, values created by
    this intrinsic are added as arguments to resume functions.
    The arguments to resume functions are the return values of the called
    coroutine.
  }];
}

def ReturnOp : LgcIlCpsOp<"return", [NoReturn]> {
  let arguments = (ins value:$returnAddr, varargs:$args);
  let results = (outs);

  let summary =
    "represents the return from a shader";

  let description = [{
    Describes the return operation for a continuation shader.

    In non-lgc.cps mode, this is used to jump to the incoming return address
    for non-RGS, and optionally passing return values in the varargs list.

    For RGS, this is used to terminate the shader after coroutine passes
    by passing an undef (non-lgc.cps mode)/poison (lgc.cps mode) address.
  }];
}
