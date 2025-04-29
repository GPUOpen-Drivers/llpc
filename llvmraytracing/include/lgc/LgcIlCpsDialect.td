/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

def SetLocalRootIndexOp : LgcIlCpsOp<"setLocalRootIndex", [WillReturn]> {
  let arguments = (ins I32:$localRootIndex);
  let results = (outs);

  let summary =
    "Sets the local root signature for the current shader";

  let description = [{
    This op accepts an i32, which is the local root signature index. This is
    used to ensure that the local root index gets properly set before any operations
    that depend on it. The arguments are:

      - localRootIndex: the local root signature index
  }];
}

def ContinueOp : LgcIlCpsOp<"continue", [NoReturn]> {
  let arguments = (ins (or I32, I64):$shaderAddr, varargs:$tail);
  let results = (outs);

  let summary =
    "represents the jump to another shader";

  let description = [{
    Describes the jump to another shader. The arguments are:

      - shaderAddr, the shader the current shader should jump to
      - tail, a set of arguments like the csp, system data or hit attributes.
  }];
}

def WaitContinueOp : LgcIlCpsOp<"waitContinue", [NoReturn]> {
  let arguments = (ins (or I32, I64):$shaderAddr, I64:$waitMask, varargs:$tail);
  let results = (outs);

  let summary =
    "represents the jump to another shader with a wait mask";

  let description = [{
    Describes the jump to another shader. The arguments are:

      - shaderAddr, the shader the current shader should jump to
      - waitMask, the bitmask all lanes have to wait for.
      - tail, a set of arguments like the csp, system data or hit attributes.
  }];
}
