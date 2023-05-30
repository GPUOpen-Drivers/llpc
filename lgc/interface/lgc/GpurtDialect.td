/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

def GpurtGetStackSize : LgcOp<"gpurt.get.stack.size", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the gpurt stack size in dword";
  let description = [{
    The dialect will return total stack size in dword of workgroup
  }];
}

def GpurtStackRead : LgcOp<"gpurt.stack.read", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins I32:$index);
  let results = (outs I32:$result);
  let summary = "read a dword from stack";
  let description = [{
    Read a dword from lds/(scratch buffer) stack at index position
  }];
}

def GpurtStackWrite : LgcOp<"gpurt.stack.write", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins I32:$index, I32:$value);
  let results = (outs I32:$result);
  let summary = "write a dword to stack";
  let description = [{
    Write a dword to lds/(scratch buffer) stack at index position
  }];
}

def GpurtGetStackBase : LgcOp<"gpurt.get.stack.base", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "this returns the stack base position in dword";
}

def GpurtGetStackStride : LgcOp<"gpurt.get.stack.stride", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the gpurt stack stride in dword";
}

def GpurtLdsStackInit : LgcOp<"gpurt.lds.stack.init", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the initial stack dword position for use with lds.stack.store";
}

def GpurtLdsStackStore : LgcOp<"gpurt.lds.stack.store", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins PrivatePointer:$new_pos, I32:$old_pos, V4I32:$data);
  let results = (outs I32:$result);
  let summary = "perform a combined lds stack push and pop operation.";
  let description = [{
    this pushes $data and pops a dword from the stack, and data and positions are interpreted according to the ds_bvh_stack_rtn instruction.
  }];
}

