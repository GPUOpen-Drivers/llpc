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

include "llvm-dialects/Dialect/Dialect.td"

def GpurtDialect : Dialect {
  let name = "lgc.gpurt";
  let cppNamespace = "lgc";
}

class GpurtOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<GpurtDialect, mnemonic_, traits_ # [NoUnwind]>;

def V2F32 : TgConstant<(FixedVectorType F32, 2)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;
def V2I32 : TgConstant<(FixedVectorType I32, 2)>, Type;
def V4I32 : TgConstant<(FixedVectorType I32, 4)>, Type;

def GpurtGetStackSizeOp : GpurtOp<"get.stack.size", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the gpurt stack size in dword";
  let description = [{
    The dialect will return total stack size in dword of workgroup
  }];
}

def GpurtStackReadOp : GpurtOp<"stack.read", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins I32:$index);
  let results = (outs I32:$result);
  let summary = "read a dword from stack";
  let description = [{
    Read a dword from lds/(scratch buffer) stack at index position
  }];
}

def GpurtStackWriteOp : GpurtOp<"stack.write", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins I32:$index, I32:$value);
  let results = (outs I32:$result);
  let summary = "write a dword to stack";
  let description = [{
    Write a dword to lds/(scratch buffer) stack at index position
  }];
}

def GpurtGetStackBaseOp : GpurtOp<"get.stack.base", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "this returns the stack base position in dword";
}

def GpurtGetStackStrideOp : GpurtOp<"get.stack.stride", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the gpurt stack stride in dword";
}

def GpurtLdsStackInitOp : GpurtOp<"lds.stack.init", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the initial stack dword position for use with lds.stack.store";
}

def GpurtLdsStackStoreOp : GpurtOp<"lds.stack.store", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins PrivatePointer:$new_pos, I32:$old_pos, V4I32:$data);
  let results = (outs I32:$result);
  let summary = "perform a combined lds stack push and pop operation.";
  let description = [{
    this pushes $data and pops a dword from the stack, and data and positions are interpreted according to the ds_bvh_stack_rtn instruction.
  }];
}

def GpurtGetBoxSortHeuristicModeOp : GpurtOp<"get.box.sort.heuristic.mode", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the box sort heuristic mode";
}

def GpurtGetStaticFlagsOp : GpurtOp<"get.static.flags", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the rayQuery/traceRay static flags";
}

def GpurtGetTriangleCompressionModeOp : GpurtOp<"get.triangle.compression.mode", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the traceRay triangle compression mode";
}

def GpurtGetFlattenedGroupThreadIdOp : GpurtOp<"get.flattened.group.thread.id", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the flattened group thread ID";
}

def GpurtSetHitAttributesOp : GpurtOp<"set.hit.attributes", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins F32:$tCurrent, I32:$kind, I32:$status, I32:$instNodeAddrLo, I32:$instNodeAddrHi,
      I32:$primitiveIndex, I32:$anyHitCallType, I32:$geometryIndex);
  let results = (outs);

  let summary = "Set hit attributes";
  let description = [{
    Compiler notification of hit attributes.

    - tCurrent: Current parametric hit distance relative to TMin.

    - kind: Intersection hit kind.

    - status: Hit status.

    - instNodeAddrLo: Current instance node address lo bits.

    - instNodeAddrHi: Current instance node address hi bits.

    - anyHitCallType: Indication of calling behavior on any hit shader.

    - geometryIndex: Current geometry index.
  }];
}

def GpurtGetHitAttributesOp
    : GpurtOp<"get.hit.attributes", [Memory<[(write ArgMem), (read InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins PointerType:$tCurrentPtr, PointerType:$kindPtr, PointerType:$statusPtr);
  let results = (outs);

  let summary = "Get hit attributes";
  let description = [{
    Get hit attributes from compiler.

    - tCurrentPtr: Pointer to store current parametric hit distance relative to TMin.

    - kindPtr: Pointer to store intersection hit kind.

    - statusPtr: Pointer to store hit status.
  }];
}

def GpurtSetTraceParamsOp : GpurtOp<"set.trace.params", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins I32:$rayFlags, I32:$instanceInclusionMask, F32:$originX, F32:$originY, F32:$originZ,
      F32:$tMin, F32:$dirX, F32:$dirY, F32:$dirZ);
  let results = (outs);

  let summary = "Set trace parameters";
  let description = [{
    Compiler notification of trace input parameters.

    - rayFlags: Ray flags.

    - instanceInclusionMask: Instance inclusion mask.

    - originX: Ray origin X.

    - originY: Ray origin Y.

    - originZ: Ray origin Z.

    - tMin: T Min.

    - dirX: World ray direction X.

    - dirY: World ray direction Y.

    - dirZ: World ray direction Z.
  }];
}

def GpurtCallClosestHitShaderOp : GpurtOp<"call.closest.hit.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shaderId, I32:$tableIndex);
  let results = (outs I1:$result);

  let summary = " Call closest hit shader";
  let description = [{
    Compiler closest hit shader inlining patch function.

    - shaderId: Closest hit shader identifier.

    - tableIndex: Hit group shader record table index.

    Returns true if the call was inlined.
  }];
}

def GpurtCallMissShaderOp : GpurtOp<"call.miss.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shaderId, I32:$tableIndex);
  let results = (outs I1:$result);

  let summary = " Call miss shader";
  let description = [{
    Compiler miss shader inlining patch function.

    - shaderId: Miss shader identifier.

    - tableIndex: Miss shader record table index.

    Returns true if the call was inlined.
  }];
}

def GpurtCallTriangleAnyHitShaderOp : GpurtOp<"call.triangle.any.hit.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shaderId, I32:$tableIndex, V2F32:$attr);
  let results = (outs);

  let summary = " Call any hit shader";
  let description = [{
    Compiler any hit shader inlining patch function.

    - shaderId: AnyHit shader identifier.

    - tableIndex: Hit group shader record index.

    - attr: Triangle barycentrics.
  }];
}

def GpurtCallIntersectionShaderOp : GpurtOp<"call.intersection.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shaderId, V2I32:$anyHitShaderId, I32:$tableIndex);
  let results = (outs);

  let summary = " Call intersection shader";
  let description = [{
    Compiler intersection shader inlining patch function.

    - shaderId: Programmable intersection shader identifier.

    - anyHitShaderId: AnyHit shader identifier.

    - tableIndex: Hit group shader record index.
  }];
}

def GpurtSetTriangleIntersectionAttributesOp
    : GpurtOp<"set.triangle.intersection.attributes", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2F32:$barycentrics);
  let results = (outs);

  let summary = "Set triangle intersection attributes";
  let description = [{
    Update the current hit attributes for a triangle hit.

    - barycentrics: Triangle barycentrics.
  }];
}

def GpurtSetHitTriangleNodePointerOp
    : GpurtOp<"set.hit.triangle.node.pointer", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins I64:$bvhAddress, I32:$nodePointer);
  let results = (outs);

  let summary = "Set hit triangle node pointer";
  let description = [{
    Compiler notification of hit triangle node pointer.

    - bvhAddress: The BVH address.

    - nodePointer: Node pointer of hit triangle.
  }];
}

def GpurtGetParentIdOp : GpurtOp<"get.parent.id", [Memory<[(read InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins);
  let results = (outs I32:$result);

  let summary = "Return parent TraceRay rayId";
}

def GpurtSetParentIdOp : GpurtOp<"set.parent.id", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins I32:$rayId);
  let results = (outs);

  let summary = "Store TraceRay rayId";
}

def GpurtSetRayStaticIdOp : GpurtOp<"set.ray.static.id", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins I32:$id);
  let results = (outs);
  let summary = "set a unique static ID for a ray";
}

def GpurtGetRayStaticIdOp : GpurtOp<"get.ray.static.id", [Memory<[(read InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "get current ray static ID";
}
