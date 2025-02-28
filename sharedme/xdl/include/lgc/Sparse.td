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

def SparsityIndexLoadOp : DivergentLgcXdlOp<"sparsityindex.load", [Memory<[(read)]>, Convergent, WillReturn]> {
  let arguments = (ins value:$pointer, value:$stride, AttrI1:$col_major, AttrI32:$memory_access);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load the sparsity index for the sparse cooperative matrix";
  let description = [{
    Load the sparsity index for sparse cooperative matrix A which will be used in sparseA * DenseB + DenseC
    Return value which size is [unused_16bit | index_16bit] for wave32 or [unused_24bit | index_8bit] for wave64.

    'pointer' is the pointer to the index data stored in memory.
    'stride' is to qualify how the index data is laid out in memory. It must be of scalar integer type.
    'col_major' is a constant instruction with 32-bit integer type whose value corresponds to a Sparsity Index Memory Layout.

    'memory_access' is a set of flags describing the memory.
      - Bit 0 is set if the memory is volatile
      - Bit 1 is set if the memory is coherent
      - Bit 2 is set if the memory is temporal.
  }];
}

def SparseCooperativeMatrixMulAddOp : DivergentLgcXdlOp<"sparseCooperativeMatrix.muladd", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix_a, value:$sparse_index, value:$matrix_b, value:$matrix_c, AttrI1:$is_signed_a, AttrI1:$is_signed_b,
                   AttrI1:$is_sat, CooperativeMatrixElementType:$matrix_a_elem_type,
                   CooperativeMatrixElementType:$matrix_b_elem_type, CooperativeMatrixElementType:$matrix_c_elem_type,
                   CooperativeMatrixElementType:$matrix_d_elem_type, AttrI32:$k_multiplier);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "The muladd operation supported as sparseA * denseB + denseC";
  let description = [{
    Sparse linear-algebraic matrix multiply of A and B with structural sparsity
    information taken from Index, followed by component-wise addition of C.
    The semantics of the multiplication are defined by the sparsity format of Index.

    Only support sparseA*DenseB+DenseC now.

    'matrix_a' is the factor cooperative matrix whose use is MatrixAKHR.
    'matrix_b' is the factor cooperative matrix whose use is MatrixBKHR.
    'sparse_index' is the sparsity index.
    'matrix_c' is the accumulator cooperative matrix whose use is MatrixCKHR.
    'is_signed_a' is the signess for matrixA's element type.
    'is_signed_b' is the signess for matrixB's element type.
    'is_sat' is the saturatingAccumulation for calculation,
    '$matrix_a_elem_type' is the component type of the A matrix.
    '$matrix_b_elem_type' is the component type of the B matrix.
    '$matrix_c_elem_type' is the component type of the C matrix.
    '$matrix_d_elem_type' is the component type of the D matrix.
    '$k_multiplier' is the multiplier for the matrix K size.
  }];
}
