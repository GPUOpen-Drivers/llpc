/*
************************************************************************************************************************
*
*  Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/

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
    let arguments = (ins ContinuationReference:$target, AttrI32:$levels, value:$state, varargs:$tail);
    let results = (outs);

    let summary = "Jump to a CPS function.";
    let description = [{
        Jump to a CPS function via:
            * target, the continuation reference
            * levels, a bitmask of levels in which target may run
            * state, which is pushed to the continuation stack before jumping,
            * an arbitrary set of arguments appended to the tail of the argument list.
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
