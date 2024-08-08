/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

def GpurtDialect : Dialect {
  let name = "lgc.gpurt";
  let cppNamespace = "lgc";
}

class GpurtOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<GpurtDialect, mnemonic_, traits_ # [NoUnwind]>;

def V2F32 : TgConstant<(FixedVectorType F32, 2)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;
def V2I32 : TgConstant<(FixedVectorType I32, 2)>, Type;
def V3I32 : TgConstant<(FixedVectorType I32, 3)>, Type;
def V4I32 : TgConstant<(FixedVectorType I32, 4)>, Type;

def PairStructType : BuiltinType {
  let arguments = (args type:$self, type:$ele_type);
  let evaluate = "::llvm::StructType::get($_context, {$ele_type, $ele_type})";
  let check = "$self->isStructTy()";
  let capture = [];
}

def StoreStackTy : TgConstant<(PairStructType I32)>, Type;

def GpurtGetStackSizeOp : GpurtOp<"get.stack.size", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the gpurt stack size in dword";
  let description = [{
    The dialect will return total stack size in dword of workgroup
  }];
}

def GpurtStackReadOp : GpurtOp<"stack.read", [Memory<[(read)]>, WillReturn]> {
  let arguments = (ins I32:$index, AttrI1:$use_extra_stack);
  let results = (outs I32:$result);
  let summary = "read a dword from stack";
  let description = [{
    Read a dword from lds/(scratch buffer) stack at index position

    Setting use_extra_stack to true means this read will perform on an extra piece of stack to avoid traversal stack conflict.
  }];
}

def GpurtStackWriteOp : GpurtOp<"stack.write", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins I32:$index, I32:$value, AttrI1:$use_extra_stack);
  let results = (outs I32:$result);
  let summary = "write a dword to stack";
  let description = [{
    Write a dword to lds/(scratch buffer) stack at index position

    Setting use_extra_stack to true means this write will perform on an extra piece of stack to avoid traversal stack conflict.
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
  let arguments = (ins AttrI1:$use_extra_stack);
  let results = (outs I32:$result);
  let summary = "return the initial stack dword position for use with lds.stack.store";
  let description = [{
    Initialize the lds/(scratch buffer) stack at index position

    Setting use_extra_stack to true means this initialize will perform on an extra piece of stack to avoid traversal stack conflict.
  }];
}

def GpurtLdsStackStoreOp : GpurtOp<"lds.stack.store", [Memory<[(write)]>, WillReturn]> {
  let arguments = (ins I32:$old_pos, I32:$last_node, V4I32:$data);
  let results = (outs StoreStackTy:$result);
  let summary = "perform a combined lds stack push and pop operation.";
  let description = [{
    lds.stack.store pushes $data onto the stack and returns a struct of a popped nodePtr and
    the new stackAddress (stack position),
    The nodePtr and stackAddress are interpreted as by the ds_bvh_stack_rtn instruction.
  }];
}

def GpurtFloatWithRoundModeOp : GpurtOp<"rt.floatop.roundmode", [Memory<[]>, WillReturn]> {
  let arguments = (ins I32:$round_mode, I32:$operation, (ScalarOrFixedVector F32):$src0, (eq $src0):$src1);
  let results = (outs (eq $src0):$result);
  let summary = "return result of floatOp with roundmode";
}

def GpurtWaveScanOp : GpurtOp<"rt.scanop.wavescan", [Memory<[]>, WillReturn]> {
  let arguments = (ins I32:$operation, I32:$flags, (ScalarOrFixedVector F32):$src0);
  let results = (outs (eq $src0):$result);
  let summary = "return result of wavescan with waveop";
}

def GpurtGetBoxSortHeuristicModeOp : GpurtOp<"get.box.sort.heuristic.mode", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the box sort heuristic mode";
}

def GpurtGetKnownSetRayFlagsOp : GpurtOp<"get.known.set.ray.flags", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the known-set bits of the traceRay specialization flags";
}

def GpurtGetKnownUnsetRayFlagsOp : GpurtOp<"get.known.unset.ray.flags", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the known-unset bits of the traceRay specialization flags";
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

def GpurtDispatchThreadIdFlatOp : GpurtOp<"dispatch.thread.id.flat", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "return the flat dispatch threadID";
}

def GpurtSetHitAttributesOp : GpurtOp<"set.hit.attributes", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins F32:$t_current, I32:$kind, I32:$status, I32:$inst_node_addr_lo, I32:$inst_node_addr_hi,
      I32:$primitiveIndex, I32:$anyHitCallType, I32:$geometryIndex);
  let results = (outs);

  let summary = "Set hit attributes";
  let description = [{
    Compiler notification of hit attributes.

    - t_current: Current parametric hit distance relative to TMin.

    - kind: Intersection hit kind.

    - status: Hit status.

    - inst_node_addr_lo: Current instance node address lo bits.

    - inst_node_addr_hi: Current instance node address hi bits.

    - anyHitCallType: Indication of calling behavior on any hit shader.

    - geometryIndex: Current geometry index.
  }];
}

def GpurtGetHitAttributesOp
    : GpurtOp<"get.hit.attributes", [Memory<[(write ArgMem), (read InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins PointerType:$t_current_ptr, PointerType:$kind_ptr, PointerType:$status_ptr);
  let results = (outs);

  let summary = "Get hit attributes";
  let description = [{
    Get hit attributes from compiler.

    - t_current_ptr: Pointer to store current parametric hit distance relative to TMin.

    - kind_ptr: Pointer to store intersection hit kind.

    - status_ptr: Pointer to store hit status.
  }];
}

def GpurtSetTraceParamsOp : GpurtOp<"set.trace.params", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins I32:$ray_flags, I32:$instance_inclusion_mask, F32:$origin_x, F32:$origin_y, F32:$origin_z,
      F32:$t_min, F32:$dir_x, F32:$dir_y, F32:$dir_z);
  let results = (outs);

  let summary = "Set trace parameters";
  let description = [{
    Compiler notification of trace input parameters.

    - ray_flags: Ray flags.

    - instance_inclusion_mask: Instance inclusion mask.

    - origin_x: Ray origin X.

    - origin_y: Ray origin Y.

    - origin_z: Ray origin Z.

    - t_min: T Min.

    - dir_x: World ray direction X.

    - dir_y: World ray direction Y.

    - dir_z: World ray direction Z.
  }];
}

def GpurtCallClosestHitShaderOp : GpurtOp<"call.closest.hit.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shader_id, I32:$table_index);
  let results = (outs I1:$result);

  let summary = " Call closest hit shader";
  let description = [{
    Compiler closest hit shader inlining patch function.

    - shader_id: Closest hit shader identifier.

    - table_index: Hit group shader record table index.

    Returns true if the call was inlined.
  }];
}

def GpurtCallMissShaderOp : GpurtOp<"call.miss.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shader_id, I32:$table_index);
  let results = (outs I1:$result);

  let summary = " Call miss shader";
  let description = [{
    Compiler miss shader inlining patch function.

    - shader_id: Miss shader identifier.

    - table_index: Miss shader record table index.

    Returns true if the call was inlined.
  }];
}

def GpurtCallTriangleAnyHitShaderOp : GpurtOp<"call.triangle.any.hit.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shader_id, I32:$table_index, V2F32:$attr);
  let results = (outs);

  let summary = " Call any hit shader";
  let description = [{
    Compiler any hit shader inlining patch function.

    - shader_id: AnyHit shader identifier.

    - table_index: Hit group shader record index.

    - attr: Triangle barycentrics.
  }];
}

def GpurtCallIntersectionShaderOp : GpurtOp<"call.intersection.shader", [Memory<[(readwrite InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins V2I32:$shader_id, V2I32:$any_hit_shader_id, I32:$table_index);
  let results = (outs);

  let summary = " Call intersection shader";
  let description = [{
    Compiler intersection shader inlining patch function.

    - shader_id: Programmable intersection shader identifier.

    - any_hit_shader_id: AnyHit shader identifier.

    - table_index: Hit group shader record index.
  }];
}

def GpurtSetHitTokenDataOp : GpurtOp<"set.hit.token.data", [Memory<[(write InaccessibleMem)]>, WillReturn]> {
  let arguments =  (ins I32:$arg0, I32:$arg1);
  let results = (outs);

  let summary = "Set hit token data";
  let description = [{
    Set hit token data.
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
  let arguments =  (ins I64:$bvh_address, I32:$node_pointer);
  let results = (outs);

  let summary = "Set hit triangle node pointer";
  let description = [{
    Compiler notification of hit triangle node pointer.

    - bvh_address: The BVH address.

    - node_pointer: Node pointer of hit triangle.
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

def GpurtGetRayStaticIdOp : GpurtOp<"get.ray.static.id", [Memory<[(read InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I32:$result);
  let summary = "get current ray static ID";
}

def GpurtContinuationStackIsGlobalOp : GpurtOp<"continuation.stack.is.global", [Memory<[]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs I1:$result);
  let summary = "Check whether continuation stack is global";
}

def GpurtGetRayQueryDispatchIdOp : GpurtOp<"get.ray.query.dispatch.id", [Memory<[(read InaccessibleMem)]>, WillReturn]> {
  let arguments = (ins);
  let results = (outs V3I32:$dispatchId);

  let summary = "Get the rayQuery dispatch ID";
  let description = [{
    This op is not called from GPURT; rather, it is generated by LowerRayQuery to get the dispatch ID to pass
    in to the GPURT initialize and proceed ops.

    For compute and ray-tracing, the rayQuery dispatch ID is the global invocation ID. For a graphics shader,
    the rayQuery dispatch ID is the subgroup local invocation ID in the X component, and 0 in the Y and Z
    components.
  }];
}
