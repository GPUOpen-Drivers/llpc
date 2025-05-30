/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

// LgcCps dialect definitions: ops used to represent continuation-specific operations

include "llvm-dialects/Dialect/Dialect.td"

def LgcCpsDialect : Dialect {
    let name = "lgc.cps";
    let cppNamespace = "lgc::cps";
}

class LgcCpsOp<string mnemonic_, list<Trait> traits_>
    : Op<LgcCpsDialect, mnemonic_, traits_>;

// A pointer to the frontend-managed continuation stack, stored in addrspace(32).
def StackPointer : TgConstant<(PointerType 32)>, Type;

// A pointer to a CPS function, combined with additional metadata
def ContinuationReference : TgConstant<(I32)>, Type;

// =====================================================================================================================
def JumpOp : LgcCpsOp<"jump", [NoReturn]> {
    let arguments = (ins ContinuationReference:$target, AttrI32:$levels, I32:$csp, I32:$shaderIndex, ContinuationReference:$rcr,varargs:$tail);
    let results = (outs);

    let summary = "Jump to a CPS function.";
    let description = [{
        Jump to a CPS function via:
            * target, the continuation reference
            * levels, a bitmask of levels in which target may run
            * csp, continuation stack pointer,
            * shader index, the shader binding table index. This index was used to lookup the
              target shader, if it is the entry function of an app shader and poison otherwise.
            * rcr, a continuation reference the called function can potentially return to
            * an arbitrary set of arguments appended to the tail of the argument list.
    }];
}

// =====================================================================================================================
def JumpUniformOp : LgcCpsOp<"jump.uniform", [NoReturn]> {
    let arguments = (ins PointerType:$target, (or I32, I64):$exec, value:$vgprs, AttrI32:$flags, varargs:$tail);
    let results = (outs);

    let summary = "Uniform jump to a CPS function.";
    let description = [{
        Uniformly jump to a CPS function via:
            * target, the target function
            * exec, the exec mask for the next function
            * vgprs, a struct consisting of arguments to be passed as VGPRs
            * flags: whether additional arguments should be passed
            * Additional arguments as described by flags.
    }];
}

// =====================================================================================================================
def AwaitOp : LgcCpsOp<"await", [NoUnwind, WillReturn]> {
    let arguments = (ins ContinuationReference:$target, AttrI32:$levels, varargs:$tail);
    let results = (outs value:$result);

    let defaultBuilderHasExplicitResultType = true;

    let summary = "Call a CPS function and await the result.";
    let description = [{
        Call a CPS function and await the result. The arguments are:
            * target, the continuation reference
            * levels, a bitmask of levels in which target may run
            * an arbitrary set of arguments appended to the tail of the argument list.
    }];
}

def CompleteOp : LgcCpsOp<"complete", [NoReturn]> {
  let arguments = (ins);
  let results = (outs);

  let summary =
    "represents lane termination for a shader";

  let description = [{
    Describes the lane termination for a shader (e. g. to end RGS).
  }];
}

// =====================================================================================================================
def AsContinuationReferenceOp : LgcCpsOp<"as.continuation.reference", [NoUnwind, WillReturn]> {
    let arguments = (ins PointerType:$fn);
    let results = (outs ContinuationReference:$ref);

    let summary = "Obtain a continuation reference from a function pointer.";
    let description = [{
        Obtain a continuation reference from a constant function pointer, $fn.
        $fn must refer to a CPS function.
    }];
}

// =====================================================================================================================
def AllocOp : LgcCpsOp<"alloc", [Memory<[(readwrite InaccessibleMem)]>, NoUnwind, WillReturn]> {
    let arguments = (ins I32:$size);
    let results = (outs StackPointer:$ptr);

    let summary = "Allocate memory on the continuation stack.";
    let description = [{
        Grow the allocation stack by $size bytes and return a pointer to the newly allocated stack space.
        $size needs to be a multiple of 4.
    }];
}

// =====================================================================================================================
def FreeOp : LgcCpsOp<"free", [Memory<[(readwrite InaccessibleMem)]>, NoUnwind, WillReturn]> {
    let arguments = (ins I32:$size);
    let results = (outs);

    let summary = "Free memory allocated on the continuation stack.";
    let description = [{
        Shrink the stack by $size number of bytes.
        The current top of the stack needs to have been reserved by a @alloc operation with the same size.
        $size needs to be a multiple of 4.
    }];
}

// =====================================================================================================================
def PeekOp : LgcCpsOp<"peek", [Memory<[(read InaccessibleMem)]>, NoUnwind, WillReturn]> {
    let arguments = (ins I32:$size);
    let results = (outs StackPointer:$ptr);

    let summary = "Peek at the top of the continuation stack.";
    let description = [{
        Return a pointer to an object of size $size at the top of the continuation stack.
        The current top of the stack needs to have been reserved by a @alloc operation with the same size.
        $size needs to be a multiple of 4.
    }];
}

// =====================================================================================================================
def SetVspOp : LgcCpsOp<"set.vsp", [Memory<[(write InaccessibleMem)]>, NoUnwind, WillReturn]> {
    let arguments = (ins StackPointer:$ptr);
    let results = (outs);

    let summary = "Set continuation stack pointer.";
    let description = [{
        Mainly used to initialize continuation stack pointer for entry-point compute shader.
    }];
}

// =====================================================================================================================
def GetVspOp : LgcCpsOp<"get.vsp", [Memory<[(read InaccessibleMem)]>, NoUnwind, WillReturn]> {
    let arguments = (ins);
    let results = (outs StackPointer:$ptr);

    let summary = "Get the pointer to the continuation stack.";
    let description = [{
        Return the current top of continuation stack pointer.
    }];
}

// =====================================================================================================================
def DeadOp : LgcCpsOp<"dead", [NoUnwind, WillReturn]> {
    let arguments = (ins);
    let results = (outs value:$out);

    let defaultBuilderHasExplicitResultType = true;

    let summary = "Return a poison value.";
    let description = [{
        This is a marker to tell the backend a value is dead.
        It is equivalent to poison. However, in contrast to poison,
        this intrinsic cannot be optimized away and helps
        the backend understand that the value is not alive.
    }];
}
