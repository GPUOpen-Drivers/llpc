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

class DivergentLgcCoopMatOp<string mnemonic_, list<Trait> traits_ = []>
    : DivergentLgcXdlOp<!strconcat("cooperative.matrix.", mnemonic_), traits_>;

class LgcCoopMatOp<string mnemonic_, list<Trait> traits_ = []>
    : LgcXdlOp<!strconcat("cooperative.matrix.", mnemonic_), traits_>;

def CooperativeMatrixLengthOp : LgcCoopMatOp<"length", [Memory<[]>, WillReturn]> {
  let arguments = (ins CooperativeMatrixLayout:$layout, AttrI32:$k_size);
  let results = (outs I32:$result);

  let summary = "get the length for the cooperative matrix";
  let description = [{
    Get the "length" of a matrix of the given layout, i.e. the number of matrix components stored per lane.

    'layout' is layout of cooperative matrix.
    'k_size' is the matrix K size.
  }];
}

def CooperativeMatrixLoadOp : DivergentLgcCoopMatOp<"load", [Memory<[(read)]>, Convergent, WillReturn]> {
  let arguments = (ins value:$pointer, value:$stride, AttrI1:$col_major, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout, AttrI32:$memory_access, AttrI32:$alignment, AttrI32:$k_size);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load the cooperative matrix elements per lane";
  let description = [{
    Load the elements of cooperative matrix per lane through a pointer.

    Return <n x i32> or <n x float> vector containing all the elements of the cooperative matrix per lane.

    'pointer' is the pointer address of the first element of the cooperative matrix stored in memory.
    'stride' is the stride in bytes in memory between the first elements in the source data.
    'col_major' is the order of the data loaded from memory, col-major or row-major.
    'elem_type' is the element type of the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.

    'memory_access' is a set of flags describing the memory.
      - Bit 0 is set if the memory is volatile
      - Bit 1 is set if the memory is coherent
      - Bit 2 is set if the memory is temporal.

    'alignment' is the alignment of this load operation.
    'k_size' is the matrix K size.
  }];
}

def CooperativeMatrixStoreOp : DivergentLgcCoopMatOp<"store", [Memory<[(write)]>, Convergent]> {
  let arguments = (ins value:$pointer, value:$stride, AttrI1:$col_major, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout, AttrI32:$memory_access, AttrI32:$alignment,
                   value:$store_value, AttrI32:$k_size);
  let results = (outs);

  let summary = "Store cooperative matrix elements per lane to the memory";
  let description = [{
    Store cooperative matrix elements per lane to the memory through the pointer. The elements should be converted to
    <n x i32> or <n x float> type.

    'pointer' is the pointer address of the data array in memory.
    'stride' is the stride in bytes in memory between the first elements in the source data.
    'col_major' is the order of the data stored into memory, col-major or row-major.
    'elem_type' is the element type of the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.

    'memory_access' is a set of flags describing the memory.
      - Bit 0 is set if the memory is volatile
      - Bit 1 is set if the memory is coherent
      - Bit 2 is set if the memory is temporal.

    'alignment' is the alignment of this store operation.
    'store_value' is the elements of the cooperative matrix perlane typed in <n x i32> or <n x float> to be stored in memory.
    'k_size' is the matrix K size.
  }];
}

def CooperativeMatrixFillOp : LgcCoopMatOp<"fill", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$scalar, CooperativeMatrixElementType:$elem_type, CooperativeMatrixLayout:$layout, AttrI32:$k_size);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Return a matrix filled with a scalar value";
  let description = [{
    Return a matrix whose elements are all equal to the given `scalar`.

    'scalar' is the value to fill the cooperative matrix.
    'elem_type' is the element type for the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.
    'k_size' is the matrix K size.
  }];
}

def CooperativeMatrixExtractOp : DivergentLgcCoopMatOp<"extract", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$matrix, value:$index, CooperativeMatrixElementType:$elem_type, CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "return the element extracted from the cooperative matrix by index";
  let description = [{
    Returns the value at the given `index` in the input matrix.

    'matrix' is the matrix from which to extract a component.
    'index' is the index to be extracted.
    'elem_type' is the element type for the cooperativ ematrix.
    'layout' is the layout of the input cooperative matrix.
  }];
}

def CooperativeMatrixInsertOp : DivergentLgcCoopMatOp<"insert", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$matrix, value:$insert_value, value:$index, CooperativeMatrixElementType:$elem_type, CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Insert the element into the cooperative matrix";
  let description = [{
    Insert the given `insert_value` at the given `index` into the input matrix and return the matrix.

    'matrix' is the matrix from which to extract a component.
    'value' is the value to be inserted.
    'index' is the index to be inserted.
    'elem_type' is the element type for the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.
  }];
}

def CooperativeMatrixConvertOp : DivergentLgcCoopMatOp<"convert", [Memory<[(read)]>, Convergent, WillReturn]> {
  let arguments = (ins AttrI32:$cast_op, value:$source, CooperativeMatrixElementType:$src_elem_type, CooperativeMatrixElementType:$dst_elem_type,
                   CooperativeMatrixLayout:$src_layout, CooperativeMatrixLayout:$dst_layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Reshape the layout for cooperative matrix or cooperative matrix element-wise-conversion operation";
  let description = [{
    This operation will convert the input matrix into either a different layout or convert the elements into a different type and return the result.

    'cast_op' is the conversion operation. 0 means reshape on cooperative matrix layout, other values are for element-wise-conversion.
    'source' is the source cooperative matrix.
    'src_elem_type' is the source cooperative matrix's element type.
    'dst_elem_type' is the destination cooperative matrix's element type.
    'src_layout' is the layout for source cooperative matrix.
    'dst_layout' is the layout for target cooperative matrix.
  }];
}

def CooperativeMatrixTransposeOp : DivergentLgcCoopMatOp<"transpose", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix, CooperativeMatrixElementType:$elem_type, CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Transpose the cooperative matrix in register level and change its layout";
  let description = [{
    This operation will transpose the input matrix and return the transposed matrix.

    'matrix' is the original cooprative matrix for transposition.
    'elem_type' is the element type for the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.
  }];
}

def CooperativeMatrixBinaryOp : DivergentLgcCoopMatOp<"binary", [Convergent, WillReturn]> {
  let arguments = (ins CooperativeMatrixArithOp:$arith_op, value:$lhs, value:$rhs, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Cooperativematrix binary operation";
  let description = [{
    Perform a binary operation on two matrices and return the resulting matrix.
    The two input matrices need to have the same layout and element type.

    'arith_op' is the arithmetic operation.
    'lhs' is the first operation of cooperative matrix.
    'rhs' is the second operation of cooperative matrix.
    'elem_type' is the element type of cooperative matrix.
    'layout' is the layout of the input cooperative matrix.
  }];
}

def CooperativeMatrixTimesScalarOp : DivergentLgcCoopMatOp<"times.scalar", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix, value:$scalar, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Compute Matrix x Scalar and return the resulting cooperative matrix";
  let description = [{
    Multiply all matrix elements in the input matrix by the given `scalar`.

    'matrix' is the matrix operand for the operation.
    'scalar' is the scalar operand for the operation.
    'elem_type' is the element type for the cooperativematrix operand.
    'layout' is the layout for the cooperative matrix.
  }];
}

def CooperativeMatrixMulAddOp : DivergentLgcCoopMatOp<"muladd", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix_a, value:$matrix_b, value:$matrix_c, AttrI1:$is_signed_a, AttrI1:$is_signed_b,
                   AttrI1:$is_sat_or_opsel, AttrI1:$is_tied, CooperativeMatrixElementType:$matrix_a_elem_type,
                   CooperativeMatrixElementType:$matrix_b_elem_type, CooperativeMatrixElementType:$matrix_c_elem_type,
                   CooperativeMatrixElementType:$matrix_d_elem_type, AttrI32:$k_multiplier);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Calculate `matrix_a` * `matrix_b` + `matrix_c`";
  let description = [{
    Multiply `matrix_a` by `matrix_b` and add `matrix_c`. The resulting matrix has the same type as `matrix_c`.

    'matrix_a' is the factor cooperative matrix whose use is MatrixAKHR.
    'matrix_b' is the factor cooperative matrix whose use is MatrixBKHR.
    'matrix_c' is the accumulator cooperative matrix whose use is MatrixAccumulatorKHR.
    'is_signed_a' is the signess for matrix_a's element type.
    'is_signed_b' is the signess for matrix_b's element type.

    'is_sat_or_opsel' is the saturatingAccumulation for calculation,
    In the case of 16-bit floating point matrices, this bit acts as an opsel bit,
    if it is set to false, we store the result in the lower half of
    the registers. If it is true, we store it in the upper half.

    'is_tied' is the flag of the output matrix has to be the same
    as the input accumulator (i.e., D has to be C)

    '$matrix_a_elem_type' is the component type of the matrix A
    '$matrix_b_elem_type' is the component type of the matrix B
    '$matrix_c_elem_type' is the component type of the matrix C
    '$matrix_d_elem_type' is the component type of the matrix D
    '$k_multiplier' is the multiplier for the matrix K size.
  }];
}

def CooperativeMatrixPackOp : DivergentLgcCoopMatOp<"pack", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins value:$matrix_c_lo, value:$matrix_c_hi);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Cooperativematrix pack operation";
  let description = [{
    This is to pack two accumulator cooperativematrices and store in the same registers.

    'matrix_c_lo' is the lower accumulator cooperative matrix to be packed.
    'matrix_c_hi' is the upper accumulator cooperative matrix to be packed.
  }];
}

def CooperativeMatrixUnPackOp : DivergentLgcCoopMatOp<"unpack", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins value:$packed_matrix, AttrI1:$get_upper_half);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Restores an unpacked matrix from a packed accumulator";
  let description = [{
    Returns the unpacked matrix stored in either the upper or lower half of a packed accumulator.

    'packed_matrix' is the packed Accumulator cooperative matrix.

    'get_upper_half' is the flag of getting the upper half or lower half of the register.
      - if it's true, it will unpack cooperative matrix stored in the upper half register.
      - if it's false, it will unpack cooperative matrix stored in the lower half register.
  }];
}
