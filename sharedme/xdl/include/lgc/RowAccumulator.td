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

class DivergentLgcCoopRowAccOp<string mnemonic_, list<Trait> traits_ = []>
    : DivergentLgcXdlOp<!strconcat("cooperative.rowacc.", mnemonic_), traits_>;

class LgcCoopRowAccOp<string mnemonic_, list<Trait> traits_ = []>
    : LgcXdlOp<!strconcat("cooperative.rowacc.", mnemonic_), traits_>;

def CooperativeRowAccLoadOp : DivergentLgcCoopRowAccOp<"load", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins value:$pointer, I32:$stride, CooperativeMatrixElementType:$elem_type, CooperativeMatrixMemoryAccess:$memory_access);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load cooperative rowacc from memory";
  let description = [{
    Load contiguous elements from the specified location of the memory.

    Return acc row data in finalized mode.

    'pointer' is the pointer address to the data.
    'stride' is the stride in bytes in memory between the first elements in the source data.
    'elem_type' is the element type for the row acc.

    'memory_access' is a set of flags describing the memory.
      - Bit 0 is set if the memory is volatile
      - Bit 1 is set if the memory is coherent
      - Bit 2 is set if the memory is temporal.
  }];
}

def CooperativeRowAccStoreOp : DivergentLgcCoopRowAccOp<"store", [Memory<[(write)]>]> {
  let arguments = (ins value:$pointer, I32:$stride, CooperativeMatrixElementType:$elem_type, value:$data, CooperativeMatrixMemoryAccess:$memory_access);
  let results = (outs);

  let summary = "store cooperative rowacc to memory";
  let description = [{
    Store a contiguous elements from the specified location of the memory.

    'pointer' is the pointer address to the data.
    'stride' is the stride in bytes in memory between the first elements in the source data.
    'elem_type' is the element type for the row acc.
    'data' is data of row acc, Must be in finalized mode.

    'memory_access' is a set of flags describing the memory.
      - Bit 0 is set if the memory is volatile
      - Bit 1 is set if the memory is coherent
      - Bit 2 is set if the memory is temporal.
  }];
}

def CooperativeRowAccAccumulateModeOp : DivergentLgcCoopRowAccOp<"accumulate.mode", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$row_acc, CooperativeMatrixElementType:$elem_type);
  let results = (outs (eq $row_acc):$result);

  let summary = "change cooperative row acc date mode from finalize mode to accumulate mode";
  let description = [{
    convert the row acc data from finalize mode to accumulate mode.

    Return acc row data in accumulate mode.

    'row_acc' is the input row acc data, must be in finalize mode.
    'elem_type' is the element type for the row acc.
  }];
}

def CooperativeRowAccFinalizeModeOp : DivergentLgcCoopRowAccOp<"finalize.mode", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$row_acc, CooperativeMatrixElementType:$elem_type);
  let results = (outs (eq $row_acc):$result);

  let summary = "change cooperative rowacc date mode from accumulate state to finalize state";
  let description = [{
    convert the row accumulator data from accumulate mode to finalize mode.

    Return row accumulator data in finalized mode.

    'row_acc' is the input row acc data, must be in accumulate mode.
    'elem_type' is the element type for the row acc.
  }];
}

def CooperativeRowAccSplatOp : DivergentLgcCoopRowAccOp<"splat", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$scalar, CooperativeMatrixElementType:$elem_type);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "fill cooperative rowacc will a scalar value";
  let description = [{
    Return filled cooperative acc row in finalize mode.

    'scalar' is the scalar value for fill the cooperative row acc.
    'elem_type' is the element type for the cooperative row acc.
  }];
}

def CooperativeRowAccSumAccumulateOp : DivergentLgcCoopRowAccOp<"sum.accumulate", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$matrix, CooperativeMatrixElementType:$matrix_elem_type, CooperativeMatrixLayout:$matrix_layout, value:$row_acc, CooperativeMatrixElementType:$row_acc_elem_type, AttrI1:$is_signed);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "sum and accumuate columns of cooperative matrix value to cooperative row accumulator";
  let description = [{
    Return accumulated acc row data in accumulate mode.

    'matrix' is the input cooperative matrix. Must be A/B matrix.
    'matrix_elem_type' is the element type for the cooperative matrix.
    'matrix_layout' is the layout for the cooperative matrix.
    'row_acc' is the input cooperative row acc, must be in accumulate mode.
    'row_acc_elem_type' is the element type for input cooperative row acc.
    'is_signed' indicate if row accumulator element type is considered signed or not.
  }];
}

def CooperativeRowAccScalarOp : DivergentLgcCoopRowAccOp<"scalar", [Memory<[]>, WillReturn]> {
  let arguments = (ins CooperativeMatrixArithOp:$binop, value:$row_acc, CooperativeMatrixElementType:$elem_type, value:$scalar, AttrI1:$accumulate_mode);
  let results = (outs (eq $row_acc):$result);

  let summary = "cooperative row accumulator scalar operation in accumulate or finalize mode";
  let description = [{
    Return the cooperative row accumulator data with same mode (accumulate or finalized) as input cooperative accumulator data.

    'binop' is the cooperative matrix arithmetic operation.
    'row_acc' is the input cooperative row accumulator.
    'elem_type' is the element type for the cooperative row accumulator data.
    'scalar' is the scalar value for operation.
    'accumulate_mode' indicate if input and return accumulator data in accumulate or finalize mode.
  }];
}

def CooperativeRowAccExpandOp : DivergentLgcCoopRowAccOp<"expand", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$row_acc, CooperativeMatrixElementType:$row_acc_elem_type, CooperativeMatrixElementType:$matrix_elem_type, CooperativeMatrixLayout:$matrix_layout, AttrI1:$col_major);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "expand cooperative row accumulator data to cooperative matrix, the input row accumulator data must be in finalize mode.";
  let description = [{
    Return the cooperative matrix.

    'row_acc' is the input cooperative row accumulator.
    'row_acc_elem_type' is the element type for the input cooperative row accumulator data.
    'matrix_elem_type' is the element type for the output cooperative matrix.
    'matrix_layout' is the element type for the output cooperative matrix.
    'col_major' indicate how to expand the cooperative row acculumlator data by row or col.
  }];
}
