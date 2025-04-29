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

def NoDivergenceSource : LlvmEnumAttributeTrait<"NoDivergenceSource", [FnTrait]>;

class DivergentLgcOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LgcDialect, mnemonic_, traits_ # [NoUnwind]>;

class LgcOp<string mnemonic_, list<Trait> traits_ = []>
    : DivergentLgcOp<mnemonic_, traits_
#ifdef LLVM_HAVE_NODIVERGENCESOURCE_ATTR
                                        # [NoDivergenceSource]
#endif
                                                              >;

def BufferAddrToPtrOp : LgcOp<"buffer.addr.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins I64:$address, AttrI1:$globallyCoherent);
  let results = (outs BufferPointer:$result);

  let summary = "convert a buffer address into a buffer fat pointer";
  let description = [{
    Given a 64-bit buffer address, returns a fat buffer pointer to the start of the buffer.

    The descriptor must be 0 or a valid address for a storage buffer aka raw buffer, i.e. a buffer
    for which the indexing feature of BUFFER_LOAD_* instructions is never used.
  }];
}

def BufferDescToPtrOp : LgcOp<"buffer.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins V4I32:$desc, AttrI1:$globallyCoherent);
  let results = (outs BufferPointer:$result);

  let summary = "convert a buffer descriptor into a buffer pointer";
  let description = [{
    Given a buffer descriptor for a storage buffer, returns a fat buffer pointer to the start of the buffer.

    The descriptor must be a null descriptor or a valid descriptor for a storage buffer aka raw buffer, i.e. a buffer
    for which the indexing feature of BUFFER_LOAD_* instructions is never used.
  }];
}

def ConvertToStridedBufferPointerOp : LgcOp<"convert.to.strided.buffer.pointer", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferPointer:$ptr, AttrI32:$stride);
  let results = (outs BufferStridedPointer:$result);

  let summary = "convert a fat buffer pointer into a strided buffer pointer";
  let description = [{
    Given a buffer pointer and a stride, returns an indexed buffer pointer to the start of the buffer.

    Adjusts the stride and number of elements in the descriptor.
  }];
}

def StridedBufferDescToPtrOp : LgcOp<"strided.buffer.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins V4I32:$desc, AttrI32:$stride, AttrI1:$globallyCoherent);
  let results = (outs BufferStridedPointer:$result);

  let summary = "convert a buffer descriptor into a strided buffer pointer";
  let description = [{
    Given a buffer descriptor for a storage buffer, returns an indexed buffer pointer to the start of the buffer.
  }];
}

def StridedBufferAddrAndStrideToPtrOp : LgcOp<"strided.buffer.addr.and.stride.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins I64:$address, I32:$stride, AttrI1:$globallyCoherent);
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

def BufferIndexOp : LgcOp<"buffer.index", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferPointer:$ptr, AttrI32:$stride, I32:$index);
  let results = (outs BufferPointer:$marked);

  let summary = "mark a runtime array for potential conversion to a strided buffer";
  let description = [{
    Mark the given array as a candidate for strided buffer pointers.
    If the runtime array represents a (RW)StructuredBuffer, we can use indexed access to its elements
    in some cases. In other cases, we prefer to manually calculate the address.

    If it is not a StructuredBuffer, we can proceed as usual.
  }];
}

def BufferLengthOp : LgcOp<"buffer.length", [Memory<[]>, WillReturn]> {
  let arguments = (ins BufferPointer:$pointer, I64:$offset);
  let results = (outs I64:$result);

  let value_traits = [
    (NoCapture $pointer)
  ];

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

  let value_traits = [
    (NoCapture $lhs),
    (NoCapture $rhs)
  ];

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
  let arguments = (ins ConstantPointer:$descPtr, AttrI1:$forceRawView, AttrI1:$isCompact, AttrI1:$globallyCoherent);
  let results = (outs BufferPointer:$result);

  let summary = "convert a constant buffer pointer to a fat buffer pointer and implicitly do the load";
  let description = [{
    Given a constant buffer pointer, load from the pointer and returns a fat buffer pointer to the start of the buffer.
  }];
}

def StridedBufferLoadDescToPtrOp : LgcOp<"stride.buffer.load.desc.to.ptr", [Memory<[]>, WillReturn]> {
  let arguments = (ins ConstantPointer:$descPtr, AttrI1:$forceRawView, AttrI1:$isCompact, AttrI1:$globallyCoherent, I32:$stride);
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

def AbortMsgOp : LgcOp<"abort.msg", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins varargs:$args);
  let results = (outs);

  let summary = "output abort messages";
  let description = [{
    Writes constant values to the metadata and variables to the debug printf buffer pointed to by `buffer`.

    Arguments must be of type `i32`.
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

def GetMeshBuiltinInputOp : DivergentLgcOp<"get.mesh.builtin.input", [Memory<[]>, WillReturn]> {
  let arguments = (ins AttrI32:$builtin);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "return the value of mesh built-in input";
  let description = [{
    Return the value of mesh built-in input.

    `builtin` is the input built-in ID of mesh shader.
  }];
}

def WriteMeshOutputOp : LgcOp<"write.mesh.output", [Memory<[]>]> {
  let arguments = (ins AttrI1:$is_primitive, AttrI32:$location, I32:$location_offset, I32:$component_index, I32:$prim_or_vertex_index, value:$output_value);
  let results = (outs);

  let summary = "write mesh shader primitive/vertex outputs";
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

def NggExportPositionOp : LgcOp<"ngg.export.position", [Memory<[(write)]>]> {
  let arguments = (ins AttrI32:$export_slot, F32:$export_value0, F32:$export_value1, F32:$export_value2, F32:$export_value3);
  let results = (outs);

  let summary = "export position in NGG primitive shader";
  let description = [{
    In NGG primitive shader, export position

    `export_slot` is the export slot.
    `export_value0` is the first position value to export.
    `export_value1` is the second position value to export.
    `export_value2` is the third position value to export.
    `export_value3` is the fourth position value to export.
  }];
}

def NggExportAttributeOp : LgcOp<"ngg.export.attribute", [Memory<[(write)]>]> {
  let arguments = (ins AttrI32:$export_slot, F32:$export_value0, F32:$export_value1, F32:$export_value2, F32:$export_value3);
  let results = (outs);

  let summary = "export attribute in NGG primitive shader";
  let description = [{
    In NGG primitive shader, export attribute

    `export_slot` is the export slot.
    `export_value0` is the first position value to export.
    `export_value1` is the second position value to export.
    `export_value2` is the third position value to export.
    `export_value3` is the fourth position value to export.
  }];
}

def WriteXfbOutputOp : LgcOp<"write.xfb.output", [Memory<[(write)]>]> {
  let arguments = (ins AttrI32:$xfb_buffer, AttrI32:$xfb_offset, AttrI32:$stream_id, value:$output_value);
  let results = (outs);

  let summary = "write XFB output to XFB buffer";
  let description = [{
    Write XFB (transform feedback) output to XFB buffer with the specified XFB offset

    `xfb_buffer` is the XFB buffer.
    `xfb_offset` is the XFB offset in this buffer.
    `stream_id` is ID of the vertex stream to write XFB output.
    `output_value` is the XFB output value to write.
  }];
}

def NggReadGsOutputOp : DivergentLgcOp<"ngg.read.gs.output", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins AttrI32:$location, AttrI32:$component, AttrI32:$stream_id);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read the value of the specified GS output from LDS";
  let description = [{
    Read the value of the specified GS output from LDS in NGG primitive shader

    `location` is location of the output.
    `component` is component of the output (used for vector element addressing).
    `stream_id` is ID of the vertex stream to read GS output.
  }];
}

def NggWriteGsOutputOp : LgcOp<"ngg.write.gs.output", [Memory<[(write)]>]> {
  let arguments = (ins AttrI32:$location, AttrI32:$component, AttrI32:$stream_id, value:$output_value);
  let results = (outs);

  let summary = "write the value of the specified GS output to LDS";
  let description = [{
    Write the value of the specified GS output to LDS in NGG primitive shader

    `location` is location of the output.
    `component` is component of the output (used for vector element addressing).
    `stream_id` is ID of the vertex stream to read GS output.
    `output_value` is the output value to write.
  }];
}

def GsEmitStreamOp : DivergentLgcOp<"gs.emit.stream", [Memory<[(write)]>]> {
let arguments = (ins AttrI32:$stream_id);
  let results = (outs);

  let summary = "emit a vertex to the current output primitive at the specified stream";
  let description = [{
    Emit a vertex to the current output primitive at the specified stream

    `stream_id` is ID of the vertex stream to emit a vertex to the current output primitive.
  }];
}

def GsCutStreamOp : DivergentLgcOp<"gs.cut.stream", [Memory<[(write)]>]> {
let arguments = (ins AttrI32:$stream_id);
  let results = (outs);

  let summary = "complete the current output primitive at the specified stream";
  let description = [{
    Complete the current output primitive at the specified stream

    `stream_id` is ID of the vertex stream to complete the current output primitive.
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

def InputImportGenericOp : DivergentLgcOp<"input.import.generic", [Memory<[]>, WillReturn]> {
  let superclass = GenericLocationOp;

  let arguments = (ins GenericLocationOp);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read a generic shader input variable";
  let description = [{
    Reads a generic graphics shader input variable from the previous stage outputs.

    Used in all graphics shader stages except vertex, task and mesh shader.
  }];
}

def LoadVertexInputOp : DivergentLgcOp<"load.vertex.input", [Memory<[]>, WillReturn]> {
  let superclass = GenericLocationOp;

  let arguments = (ins GenericLocationOp, I32:$vertex_id, I32:$instance_id, I32:$draw_id, I32:$base_vertex, I32:$base_instance);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read from a vertex buffer input";
  let description = [{
    Reads a generic input variable from a vertex buffer according to vertex array and buffer bindings.

    Used in vertex shaders and the transform compute shader (which inlines a vertex shader).
  }];
}

// TODO: Consider restricting the memory effect to inaccessible memory only.
def OutputImportGenericOp : DivergentLgcOp<"output.import.generic", [Memory<[(read)]>, WillReturn]> {
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

def EvalIjOffsetSmoothOp : DivergentLgcOp<"eval.Ij.offset.smooth", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$value);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "evaluate I,J for interpolation: center offset, smooth (perspective) version";
  let description = [{
    Evaluate I,J for interpolation: center offset, smooth (perspective) version.

    Only used in Fragment.
  }];
}

def AdjustIjOp : DivergentLgcOp<"adjust.Ij", [Memory<[]>, WillReturn]> {
  let arguments = (ins value:$value, value:$offset);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "Adjust I,J values by offset.";
  let description = [{
    This adjusts value by its X and Y derivatives times the X and Y components of offset.
    If value is a vector, this is done component-wise.
  }];
}

def InputImportInterpolatedOp : DivergentLgcOp<"input.import.interpolated", [Memory<[]>, WillReturn]> {
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

def LoadTfeOp : LgcOp<"load.tfe", [Memory<[]>, WillReturn]> {
  let arguments = (ins (or BufferPointer, BufferStridedPointer):$pointer);
  let results = (outs value:$result);

  let value_traits = [
    (NoCapture $pointer)
  ];

  let defaultBuilderHasExplicitResultType = true;

  let summary = "load data and tfe value from the given buffer pointer";
  let description = [{
    Return the data stored in the structure or raw buffer and the tfe value in term of {T, i32}.
  }];
}

def EmitShaderHashTokenOp : LgcOp<"emit.shader.hash.token", [Memory<[]>]> {
  let arguments = (ins);
  let results = (outs);

  let summary = "emit SQTT token with API shader hash";
  let description = [{
    Emits s_ttracedata with shader hash generated at runtime to export the data to SQTT.
  }];
}
