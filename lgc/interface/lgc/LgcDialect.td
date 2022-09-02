/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

include "llvm-dialects/Dialect/Dialect.td"

def LgcDialect : Dialect {
  let name = "lgc";
  let cppNamespace = "lgc";
}

def BufferPointer : TgConstant<(PointerType 7)>, Type;

def V4I32 : TgConstant<(FixedVectorType I32, 4)>, Type;

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

def GenericLocationOp : OpClass<LgcDialect> {
  let arguments = (ins AttrI1:$perPrimitive, AttrI32:$location, I32:$locOffset, I32:$elemIdx, I32:$arrayIndex);

  let summary = "family of operations that reference generic shader I/O locations";
  let description = [{
    Generic (graphics) shader I/O interfaces are addressed in terms of locations. Locations are 4-element vectors with
    32-bit elements. Within an interface, the location vectors are indexed by `location + locOffset`. The elements of
    the vector are indexed by `elemIdx`.

    For some shader stages, the interface is inherently an array. For example, GS inputs are an array of per-vertex
    inputs, where the size of the array depends on the input primitive type (1 for points, 2 for lines, etc.).

    Some shader stages have both array and non-array interfaces. Specifically, TCS outputs and TES inputs have per-patch
    locations (non-array) and per-control-point locations (array with one entry per control point).

    For array interfaces, `arrayIndex` is used to index into the array, e.g. arrayIndex is the control point index (in
    TCS outputs and TES inputs) or the vertex index (in GS inputs). For non-array interfaces, `arrayIndex` is ignored
    entirely and is recommended to be set to `poison`.

    Some shader stages have a notion of inputs or outputs that are "per primitive":
    - TCS outputs: per-patch vs. per-control-point
    - TES inputs: per-patch vs. per-control-point
    - MESH outputs: per-primitive vs. per-vertex
    - PS inputs: per-primitive vs. per-vertex/interpolated (note: old-school flat-interpolated inputs are considered
      to be interpolated)

    TODO: Tease out the exact rules for when `locOffset` is allowed to be non-0. For inputs, this is originally only
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

  let arguments = (ins GenericLocationOp, AttrI32:$interpMode, value:$interpValue);
  let results = (outs value:$result);

  let defaultBuilderHasExplicitResultType = true;

  let summary = "read a generic per-vertex (interpolated) pixel shader input";
  let description = [{
    Only used in PS for per-vertex/interpolated inputs. Use `input.import.generic` for per-primitive inputs.

    `interpMode` is one of:
    
    - InterpModeSmooth for interpolation using the `<2 x float>` barycentrics in `interpValue`
    - InterpModeFlat for flat shading; `interpValue` is ignored and is recommended to be `poison`
    - InterpModeCustom to retrieve the attribute of the vertex with the `i32` index `interpValue` (which must be 0, 1,
      or 2). The raw HW vertex index is used, which may be different from the API vertex index; it is up to the user of
      this operation to map between HW and API.
  }];
}
