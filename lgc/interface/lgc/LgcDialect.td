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

include "llvm-dialects/Dialect/Dialect.td"

def LgcDialect : Dialect {
  let name = "lgc";
  let cppNamespace = "lgc";
}

def ConstantPointer : TgConstant<(PointerType 4)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;
def BufferPointer : TgConstant<(PointerType 7)>, Type;
def BufferStridedPointer : TgConstant<(PointerType 9)>, Type;
def TaskPayloadPointer : TgConstant<(PointerType 7)>, Type;

def V4I32 : TgConstant<(FixedVectorType I32, 4)>, Type;

defm CooperativeMatrixMemoryAccess : AttrEnum<"CooperativeMatrixMemoryAccess">;
defm CooperativeMatrixElementType : AttrEnum<"CooperativeMatrixElementType">;
defm CooperativeMatrixLayout : AttrEnum<"CooperativeMatrixLayout">;
defm CooperativeMatrixArithOp : AttrEnum<"CooperativeMatrixArithOp">;

class LgcOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LgcDialect, mnemonic_, traits_ # [NoUnwind]>;

def BufferDescToPtrOp : LgcOp<"buffer.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins V4I32:$desc);
  let results = (outs BufferPointer:$result);

  let summary = "convert a buffer descriptor into a buffer pointer";
  let description = [{
    Given a buffer descriptor for a storage buffer, returns a fat buffer pointer to the start of the buffer.

    The descriptor must be a null descriptor or a valid descriptor for a storage buffer aka raw buffer, i.e. a buffer
    for which the indexing feature of BUFFER_LOAD_* instructions is never used.
  }];
}

def StridedBufferDescToPtrOp : LgcOp<"strided.buffer.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins V4I32:$desc);
  let results = (outs BufferStridedPointer:$result);

  let summary = "convert a buffer descriptor into a strided buffer pointer";
  let description = [{
    Given a buffer descriptor for a storage buffer, returns an indexed buffer pointer to the start of the buffer.
  }];
}

def StridedBufferAddrAndStrideToPtrOp : LgcOp<"strided.buffer.addr.and.stride.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins I64:$address, I32:$stride);
  let results = (outs BufferStridedPointer:$result);

  let summary = "convert a buffer address and a stride into a strided buffer pointer";
  let description = [{
    Given a buffer address and stride, returns an indexed buffer pointer to the start of the buffer.
  }];
}

def StridedIndexAddOp : LgcOp<"strided.index.add", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferStridedPointer:$ptr, I32:$delta_idx);
  let results = (outs BufferStridedPointer:$result);

  let summary = "add to the index of a strided buffer pointer";
  let description = [{
    Given a strided buffer pointer and a delta index, adds the delta index to the current index
    and returns a pointer to the location pointed to by the new index.
  }];
}

def BufferLengthOp : LgcOp<"buffer.length", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferPointer:$pointer, I32:$offset);
  let results = (outs I32:$result);

  let summary = "return the size of a buffer";
  let description = [{
    Return the number of bytes available in the buffer pointed to by `pointer`, starting at a byte `offset` from the
    pointer.

    `offset` is interpreted as an unsigned integer. If `offset` is greater than the total size of the buffer, or if the
    buffer is a null buffer, behavior depends on the `allowNullDescriptors` setting:
    - if null descriptors are allowed, the offset is properly clamped and 0 is returned.
    - if null descriptors are *not* allowed, the return value is poison.
  }];
}

def BufferPtrDiffOp : LgcOp<"buffer.ptr.diff", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferPointer:$lhs, BufferPointer:$rhs);
  let results = (outs I64:$result);

  let summary = "return the difference between buffer pointers in bytes";
  let description = [{
    Return the (signed) distance `lhs - rhs` from rhs to lhs in bytes.

    If `lhs` and `rhs` have different pointer provenance, the result is poison.

    If `lhs` and `rhs` are both null, the result is 0.
  }];
}

def LoadBufferDescOp : LgcOp<"load.buffer.desc", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI64:$desc_set, AttrI32:$binding, I32:$desc_index,
      AttrI32:$flags);
  let results = (outs BufferPointer:$result);

  let summary = "create a load of a buffer descriptor";
  let description = [{
    Return the buffer descriptor pointer.

    `flags` must not contain `BufferFlagAddress` for this Op.
  }];
}

def DebugPrintfOp : LgcOp<"debug.printf", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins BufferPointer:$buffer, ConstantPointer:$format, varargs:$args);
  let results = (outs);

  let summary = "print a formatted message";
  let description = [{
    Writes an entry to the debug printf buffer pointed to by `buffer`. No-op if `buffer` is literal constant poison.

    `format` must be a non-external global variable in the constant address space of type `[n x i8]` (not
    null-terminated).
  }];
}

def TaskPayloadPtrOp : LgcOp<"task.payload.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs TaskPayloadPointer:$payload);

  let summary = "return the pointer to task payload";
  let description = [{
    Return the point to task payload of task shader.

    The returned task payload pointer is further used by subsequent read/write/atomics operations on task payload.
  }];
}

def EmitMeshTasksOp : LgcOp<"emit.mesh.tasks", [Memory<[]>]> {
  let arguments = (ins I32:$group_count_x, I32:$group_count_y, I32:$group_count_z);
  let results = (outs);

  let summary = "emit the current values of all per-task output variables to the current task output";
  let description = [{
    In the task shader, emit the current values of all per-task output variables to the current task output by
    specifying the group count XYZ of the launched child mesh tasks.

    `group_count_x` is X dimension of the launched child mesh tasks.
    `group_count_y` is Y dimension of the launched child mesh tasks.
    `group_count_z` is Z dimension of the launched child mesh tasks.
  }];
}

def SetMeshOutputsOp : LgcOp<"set.mesh.outputs", [Memory<[]>]> {
  let arguments = (ins I32:$vertex_count, I32:$primitive_count);
  let results = (outs);

  let summary = "set the actual output size of the primitives and vertices that the mesh shader workgroup will emit";
  let description = [{
    In the mesh shader, set the actual output size of the primitives and vertices that the mesh shader workgroup will
    emit upon completion.

    `vertex_count` is the actual output size of the vertices.
    `primitive_count` is the actual output size of the primitives.
  }];
}

def SetMeshPrimitiveIndicesOp : LgcOp<"set.mesh.primitive.indices", [Memory<[]>]> {
  let arguments = (ins I32:$primitive_index, (ScalarOrFixedVector I32):$primitive_indices);
  let results = (outs);

  let summary = "set primitive indices for mesh shader";
  let description = [{
    In the mesh shader, set primitive indices by forming primitive connectivity data and writing it to LDS.

    `primitive_index` is the primitive index specifying which primitive to set.
    `primitive_indices` are all vertex index values that are used to form this primitive.
  }];
}

def SetMeshPrimitiveCulledOp : LgcOp<"set.mesh.primitive.culled", [Memory<[]>]> {
  let arguments = (ins I32:$primitive_index, I1:$is_culled);
  let results = (outs);

  let summary = "set primitive culled state for mesh shader";
  let description = [{
    In the mesh shader, set primitive culled state by writing the null primitive flag to LDS.

    `primitive_index` is the primitive index specifying which primitive to set.
    `is_culled` is a boolean flag indicating whether this primitive is culled.
  }];
}

def GetMeshBuiltinInputOp : LgcOp<"get.mesh.builtin.input", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI32:$builtin);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "return the value of mesh built-in input";
  let description = [{
    Return the value of mesh built-in input.

    `builtIn` is the input built-in ID of mesh shader.
  }];
}

def WriteMeshVertexOutputOp : LgcOp<"write.mesh.vertex.output", [Memory<[]>]> {
  let arguments = (ins I32:$output_offset, I32:$vertex_index, value:$output_value);
  let results = (outs);

  let summary = "Write mesh shader vertex outputs";
  let description = [{
    In the mesh shader, write mesh shader vertex outputs to LDS.

    `output_offset` is the relative offset of this output (in dwords) within all outputs of the indexed vertex.
    `vertex_index` is the vertex index specifying which vertex to write.
    `output_value` is the output value to write.
  }];
}

def WriteMeshPrimitiveOutputOp : LgcOp<"write.mesh.primitive.output", [Memory<[]>]> {
  let arguments = (ins I32:$output_offset, I32:$primitive_index, value:$output_value);
  let results = (outs);

  let summary = "Write mesh shader primitive outputs";
  let description = [{
    In the mesh shader, write mesh shader primitive outputs to LDS.

    `output_offset` is the relative offset of this output (in dwords) within all outputs of the indexed primitive.
    `primitive_index` is the primitive index specifying which primitive to write.
    `output_value` is the output value to write.
  }];
}

def GenericLocationOp : OpClass<LgcDialect> {
  let arguments = (ins AttrI1:$per_primitive, AttrI32:$location, I32:$loc_offset, I32:$elem_idx, I32:$array_index);

  let summary = "family of operations that reference generic shader I/O locations";
  let description = [{
    Generic (graphics) shader I/O interfaces are addressed in terms of locations. Locations are 4-element vectors with
    32-bit elements. Within an interface, the location vectors are indexed by `location + loc_offset`. The elements of
    the vector are indexed by `elem_idx`.

    For some shader stages, the interface is inherently an array. For example, GS inputs are an array of per-vertex
    inputs, where the size of the array depends on the input primitive type (1 for points, 2 for lines, etc.).

    Some shader stages have both array and non-array interfaces. Specifically, TCS outputs and TES inputs have per-patch
    locations (non-array) and per-control-point locations (array with one entry per control point).

    For array interfaces, `array_index` is used to index into the array, e.g. array_index is the control point index (in
    TCS outputs and TES inputs) or the vertex index (in GS inputs). For non-array interfaces, `array_index` is ignored
    entirely and is recommended to be set to `poison`.

    Some shader stages have a notion of inputs or outputs that are "per primitive":
    - TCS outputs: per-patch vs. per-control-point
    - TES inputs: per-patch vs. per-control-point
    - MESH outputs: per-primitive vs. per-vertex
    - PS inputs: per-primitive vs. per-vertex/interpolated (note: old-school flat-interpolated inputs are considered
      to be interpolated)

    TODO: Tease out the exact rules for when `loc_offset` is allowed to be non-0. For inputs, this is originally only
    TCS, TES, and PS in special cases.
  }];
}

def InputImportGenericOp : LgcOp<"input.import.generic", [Memory<[]>, WillReturn]> {
  let superclass = GenericLocationOp;

  let arguments = (ins GenericLocationOp);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read a generic shader input variable";
  let description = [{
    Reads a generic graphics shader input variable, either from a vertex buffer or from the previous stage outputs.

    Used in all graphics shader stages except task and mesh shader.
  }];
}

// TODO: Consider restricting the memory effect to inaccessible memory only.
def OutputImportGenericOp : LgcOp<"output.import.generic", [Memory<[(read)]>, WillReturn]> {
  let superclass = GenericLocationOp;

  let arguments = (ins GenericLocationOp);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read back a generic shader output variable";
  let description = [{
    Reads back a generic shader output variable.

    Only used in TCS, where outputs can be used to communicate between multiple invocations.
  }];
}

def InputImportInterpolatedOp : LgcOp<"input.import.interpolated", [Memory<[]>, WillReturn]> {
  let superclass = GenericLocationOp;

  let arguments = (ins GenericLocationOp, AttrI32:$interp_mode, value:$interp_value);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read a generic per-vertex (interpolated) pixel shader input";
  let description = [{
    Only used in PS for per-vertex/interpolated inputs. Use `input.import.generic` for per-primitive inputs.

    `interp_mode` is one of:

    - InterpModeSmooth for interpolation using the `<2 x float>` barycentrics in `interp_value`
    - InterpModeFlat for flat shading; `interp_value` is ignored and is recommended to be `poison`
    - InterpModeCustom to retrieve the attribute of the vertex with the `i32` index `interp_value` (which must be 0, 1,
      or 2). The raw HW vertex index is used, which may be different from the API vertex index; it is up to the user of
      this operation to map between HW and API.
  }];
}

def LoadUserDataOp : LgcOp<"load.user.data", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI32:$offset);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let verifier = [
    (or (I32 $result),
        (I64 $result),
        (FixedVectorType $result, I32, any)),
  ];

  let summary = "load from a constant offset in the user data";
  let description = [{
    `offset` is the offset into the user data table, in bytes. It must be a multiple of 4.
  }];
}

def UserDataOp : LgcOp<"user.data", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI32:$offset);
  let results = (outs ConstantPointer:$result);

  let summary = "return a pointer into user data";
  let description = [{
    `offset` is a byte offset into user data

    Attempting to access user data before `offset` via a pointer returned by this operation is undefined behavior.

    This operation is used for push constants in Vulkan and in some cases by OpenGL.
  }];
}

def GroupMemcpyOp : LgcOp<"group.memcpy", [Memory<[]>]> {
  let arguments = (ins PointerType:$dst, PointerType:$src, AttrI32:$size, AttrI32:$scope);
  let results = (outs);

  let summary = "copy a memory area cooperatively using the threads of a group";
  let description = [{
    Only usable in compute-like shader types (compute, task, mesh).

    Use all threads of a group (workgroup or subgroup aka wave) to copy `size` bytes
    from `src` to `dst`.

    `dst`, `src`, and `size` must be uniform at the given scope.

    `scope` is 2 for workgroup scope and 3 for subgroup (wave) scope. No other values are allowed.

    This operation must only occur in control flow that is uniform for the relevant scope.
  }];
}

def SubgroupElectOp : LgcOp<"subgroup.elect", [NoUnwind, Convergent]> {
  let arguments = (ins);
  let results = (outs I1:$result);

  let summary = "subgroupElect";
  let description = [{
    This is true only in the active invocation with the lowest id in the group,
    otherwise `result` is false.

    This is used to implement OpGroupNonUniformElect in the SPIR-V reference,
    see there for details.
  }];
}

def SubgroupAnyOp : LgcOp<"subgroup.any", [NoUnwind, Convergent]> {
  let arguments = (ins I1:$value);
  let results = (outs I1:$result);

  let summary = "subgroupAny";
  let description = [{
    Evaluates `value` for all invocations in the group. `result` will be true if
    and only if it evaluates to true for any invocation in the group.

    This is used to implement OpGroupAny and OpGroupNonUniformAny in the SPIR-V
    reference, see there for details.
  }];
}

def CooperativeRowAccLoadOp : LgcOp<"cooperative.rowacc.load", [Memory<[(read)]>, WillReturn]> {
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
    'memory_access' is the memory operands which provide:isVolatile/isTemporal/isCoherent additional operands,
    maybe volatile/Aligned/Nontemporal/MakePointerAvailable
  }];
}

def CooperativeRowAccStoreOp : LgcOp<"cooperative.rowacc.store", [Memory<[(write)]>]> {
  let arguments = (ins value:$pointer, I32:$stride, CooperativeMatrixElementType:$elem_type, value:$data, CooperativeMatrixMemoryAccess:$memory_access);
  let results = (outs);

  let summary = "store cooperative rowacc to memory";
  let description = [{
    Store a contiguous elements from the specified location of the memory.

    'pointer' is the pointer address to the data.
    'stride' is the stride in bytes in memory between the first elements in the source data.
    'elem_type' is the element type for the row acc.
    'data' is data of row acc, Must be in finalized mode.
    'memory_access' is the memory operands which provide:isVolatile/isTemporal/isCoherent additional operands,
    maybe volatile/Aligned/Nontemporal/MakePointerAvailable
  }];
}

def CooperativeRowAccAccumulateModeOp : LgcOp<"cooperative.rowacc.accumulate.mode", [Memory<[]>, WillReturn]> {
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

def CooperativeRowAccFinalizeModeOp : LgcOp<"cooperative.rowacc.finalize.mode", [Memory<[]>, WillReturn]> {
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

def CooperativeRowAccSplatOp : LgcOp<"cooperative.rowacc.splat", [Memory<[]>, WillReturn]> {
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

def CooperativeRowAccSumAccumulateOp : LgcOp<"cooperative.rowacc.sum.accumulate", [Memory<[]>, WillReturn]> {
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
    'is_signed' indicate if row accumulator elememnt type need to be consider as signed or not.
  }];
}

def CooperativeRowAccScalarOp : LgcOp<"cooperative.rowacc.scalar", [Memory<[]>, WillReturn]> {
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

def CooperativeRowAccExpandOp : LgcOp<"cooperative.rowacc.expand", [Memory<[]>, WillReturn]> {
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
