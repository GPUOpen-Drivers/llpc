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

def BufferAddrToPtrOp : LgcOp<"buffer.addr.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins I64:$address);
  let results = (outs BufferPointer:$result);

  let summary = "convert a buffer address into a buffer fat pointer";
  let description = [{
    Given a 64-bit buffer address, returns a fat buffer pointer to the start of the buffer.

    The descriptor must be 0 or a valid address for a storage buffer aka raw buffer, i.e. a buffer
    for which the indexing feature of BUFFER_LOAD_* instructions is never used.
  }];
}

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

def LoadStridedBufferDescOp : LgcOp<"load.strided.buffer.desc", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI64:$desc_set, AttrI32:$binding, I32:$desc_index, AttrI32:$flags, AttrI32:$stride);
  let results = (outs BufferStridedPointer:$result);

  let summary = "create a load of a strided buffer descriptor";
  let description = [{
    Return the strided buffer descriptor pointer.

    'stride' If stride is 0, then the descriptor must be a full descriptor in memory (can't be inline or compact), and must be a strided descriptor.
    Otherwise, the descriptor must either be a full strided descriptor in memory, in which case the descriptor's stride is used;
    alternatively, the descriptor can be an inline or compact buffer, in which case stride is used for the stride of the returned buffer.

    `flags` must not contain `BufferFlagAddress` for this Op.
  }];
}

def BufferLoadDescToPtrOp : LgcOp<"buffer.load.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins ConstantPointer:$descPtr, AttrI1:$forceRawView, AttrI1:$isCompact);
  let results = (outs BufferPointer:$result);

  let summary = "convert a constant buffer pointer to a fat buffer pointer and implicitly do the load";
  let description = [{
    Given a constant buffer pointer, load from the pointer and returns a fat buffer pointer to the start of the buffer.
  }];
}

def StridedBufferLoadDescToPtrOp : LgcOp<"stride.buffer.load.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins ConstantPointer:$descPtr, AttrI1:$forceRawView, AttrI1:$isCompact, I32:$stride);
  let results = (outs BufferStridedPointer:$result);

  let summary = "convert a constant buffer pointer to a fat strided buffer pointer and implicitly do the load";
  let description = [{
    Given a constant buffer pointer, load from the buffer pointer and returns a fat strided buffer descriptor pointer to the start of the buffer.
  }];
}

def DebugPrintfOp : LgcOp<"debug.printf", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins ImmutableStringAttr:$format, varargs:$args);
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

def WriteMeshOutputOp : LgcOp<"write.mesh.output", [Memory<[]>]> {
  let arguments = (ins AttrI1:$is_primitive, AttrI32:$location, I32:$location_offset, I32:$component_index, I32:$prim_or_vertex_index, value:$output_value);
  let results = (outs);

  let summary = "Write mesh shader primitive/vertex outputs";
  let description = [{
    In the mesh shader, write mesh shader primitive/vertex outputs to LDS.

    `is_primitive` indicates if this write is for a primitive output or for a vertex output.
    `location` is the start location of this output.
    `location_offset` is the relative location offset of this output, used by arrayed outputs.
    `component_index` is the component index of this output when component addressing is involved.
    `prim_or_vertex_index` is the primitive/vertex index specifying which primitive/vertex to write.
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

def PopsBeginInterlockOp : LgcOp<"pops.begin.interlock", [WillReturn]> {
  let arguments = (ins);
  let results = (outs);

  let summary = "begin a POPS critical section";
  let description = [{
    Only used in PS to begin a critical section of primitive ordered pixel shading (POPS).
  }];
}

def PopsEndInterlockOp : LgcOp<"pops.end.interlock", [WillReturn]> {
  let arguments = (ins);
  let results = (outs);

  let summary = "end a POPS critical section";
  let description = [{
    Only used in PS to end a critical section of primitive ordered pixel shading (POPS).
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

def ExtendAddressOp : LgcOp<"extend.address", [Memory<[]>, WillReturn]> {
  let arguments = (ins I32:$addr32);
  let results = (outs I64:$addr64);

  let summary = "extend a 32-bit address to 64 bit";
  let description = [{
    The 32-bit address is extended to 64 bits by whatever method LGC is configured to use, which
    could be using s_getpc to get the high half, or using a device-wide constant it was given.
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

def SubgroupAllOp : LgcOp<"subgroup.all", [NoUnwind, Convergent]> {
  let arguments = (ins I1:$value);
  let results = (outs I1:$result);

  let summary = "subgroupAll";
  let description = [{
    Evaluates `value` for all active invocations in the group. `result` will be
    true if and only if it evaluates to true for all invocations in the group.

    This is used to implement OpGroupAll OpGroupNonUniformAll in the SPIR-V
    reference, see there for details.
  }];
}

def SubgroupAllEqualOp : LgcOp<"subgroup.all.equal", [NoUnwind, Convergent]> {
  let arguments = (ins value:$value);
  let results = (outs I1:$result);

  let summary = "subgroupAllEqual";
  let description = [{
    Evaluates `value` for all active invocations in the group. `result` will be
    true if and only if it is equal for all active invocations in the group

    This is used to implement to OpGroupAllEqual and OpGroupNonUniformAllEqual
    in the SPIR-V reference, see there for details.
  }];
}

def SubgroupRotateOp : LgcOp<"subgroup.rotate", [NoUnwind, Convergent]> {
  let arguments = (ins value:$value, I32:$delta, I32:$clusterSize);
  let results = (outs (eq $value):$result);

  let summary = "subgroupRotate";
  let description = [{
    Reserved for SPIR-V OpGroupNonUniformRotateKHR.

    clusterSize is optional. If it is unset it will be represented by a
    llvm::PoisonValue
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

	'memory_access' is a set of flags describing the memory.
	- Bit 0 is set if the memory is volatile
	- Bit 1 is set if the memory is coherent
	- Bit 2 is set if the memory is temporal.
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

	'memory_access' is a set of flags describing the memory.
	- Bit 0 is set if the memory is volatile
	- Bit 1 is set if the memory is coherent
	- Bit 2 is set if the memory is temporal.
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
    'is_signed' indicate if row accumulator element type is considered signed or not.
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

def LoadDriverTableEntryOp : LgcOp<"load.driver.table.entry", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI32:$offset);
  let results = (outs value:$entry);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load a driver table entry from specified offset.";
  let description = [{
    The driver table is setup by PAL and contains global information such as descriptors for tessellation ring buffers, attributes-through-memory buffer, etc.
    `offset` is the offset into the driver table, in unit of dwords.
  }];
}

def InvariantDecorationOp : LgcOp<"invariant.decoration", [WillReturn]> {
  let arguments = (ins value:$invariant);
  let results = (outs);

  let summary = "Indication that the value has been marked with invariant decoration.";
  let description = [{
    Value marked with invariant decoration will have allowContract FMF set to false to prevent
    FMA contraction in the backend.
  }];
}

def CooperativeMatrixLengthOp : LgcOp<"cooperative.matrix.length", [Memory<[]>, WillReturn]> {
  let arguments = (ins CooperativeMatrixLayout:$layout);
  let results = (outs I32:$result);

  let summary = "get the length for the cooperative matrix";
  let description = [{
    Get the "length" of a matrix of the given layout, i.e. the number of matrix components stored per lane.

    'layout' is layout of cooperative matrix.
  }];
}

def CooperativeMatrixLoadOp : LgcOp<"cooperative.matrix.load", [Memory<[(read)]>, Convergent, WillReturn]> {
  let arguments = (ins value:$pointer, value:$stride, AttrI1:$col_major, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout, AttrI32:$memory_access, AttrI32:$alignment);
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
  }];
}

def CooperativeMatrixStoreOp : LgcOp<"cooperative.matrix.store", [Memory<[(write)]>, Convergent]> {
  let arguments = (ins value:$pointer, value:$stride, AttrI1:$col_major, CooperativeMatrixElementType:$elem_type,
                   CooperativeMatrixLayout:$layout, AttrI32:$memory_access, AttrI32:$alignment,
                   value:$store_value);
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
  }];
}

def CooperativeMatrixFillOp : LgcOp<"cooperative.matrix.fill", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$scalar, CooperativeMatrixElementType:$elem_type, CooperativeMatrixLayout:$layout);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Return a matrix filled with a scalar value";
  let description = [{
    Return a matrix whose elements are all equal to the given `scalar`.

    'scalar' is the value to fill the cooperative matrix.
    'elem_type' is the element type for the cooperative matrix.
    'layout' is the layout of the input cooperative matrix.
  }];
}

def CooperativeMatrixExtractOp : LgcOp<"cooperative.matrix.extract", [Memory<[]>, WillReturn]> {
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

def CooperativeMatrixInsertOp : LgcOp<"cooperative.matrix.insert", [Memory<[]>, WillReturn]> {
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

def CooperativeMatrixConvertOp : LgcOp<"cooperative.matrix.convert", [Memory<[(read)]>, Convergent, WillReturn]> {
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

def CooperativeMatrixTransposeOp : LgcOp<"cooperative.matrix.transpose", [Convergent, WillReturn]> {
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

def CooperativeMatrixBinaryOp : LgcOp<"cooperative.matrix.binary", [Convergent, WillReturn]> {
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

def CooperativeMatrixTimesScalarOp : LgcOp<"cooperative.matrix.times.scalar", [Convergent, WillReturn]> {
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

def CooperativeMatrixMulAddOp : LgcOp<"cooperative.matrix.muladd", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix_a, value:$matrix_b, value:$matrix_c, AttrI1:$is_signed_a, AttrI1:$is_signed_b,
                   AttrI1:$is_sat_or_opsel, AttrI1:$is_tied, CooperativeMatrixElementType:$accu_elem_type,
                   CooperativeMatrixElementType:$factor_elem_type);
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

    'accu_elem_type' is the component type of the accumulator matrix.
    'factor_elem_type' is the component type of the factor matrix.
  }];
}

def CooperativeMatrixPackOp : LgcOp<"cooperative.matrix.pack", [Memory<[(read)]>, WillReturn]> {
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

def CooperativeMatrixUnPackOp : LgcOp<"cooperative.matrix.unpack", [Memory<[(read)]>, WillReturn]> {
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

def SparsityIndexLoadOp : LgcOp<"sparsityindex.load", [Memory<[(read)]>, Convergent, WillReturn]> {
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

def SparseCooperativeMatrixMulAddOp : LgcOp<"sparseCooperativeMatrix.muladd", [Convergent, WillReturn]> {
  let arguments = (ins value:$matrix_a, value:$sparse_index, value:$matrix_b, value:$matrix_c, AttrI1:$is_signed_a, AttrI1:$is_signed_b,
                   AttrI1:$is_sat, CooperativeMatrixElementType:$accu_elem_type,
                   CooperativeMatrixElementType:$factor_elem_type);
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
    'accu_elem_type' is the component type of the accumulator matrix.
    'factor_elem_type' is the component type of the factor matrix.
  }];
}

def LoadTfeOp : LgcOp<"load.tfe", [Memory<[]>, WillReturn]> {
  let arguments = (ins (or BufferPointer, BufferStridedPointer):$pointer);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load data and tfe value from the given buffer pointer";
  let description = [{
    Return the data stored in the structure or raw buffer and the tfe value in term of {T, i32}.
  }];
}
