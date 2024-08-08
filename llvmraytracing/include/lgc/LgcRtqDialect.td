/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// lgc.rtq dialect: a dialect to represent ray query objects and operations on them
//
// Ray query objects are allocated via `alloca` in the usual address space for function-local variables. For now,
// they are represented as an i127. All ops operate on pointers to ray query objects, which is why we currently don't
// define a ray query type in TableGen; but there are C++ helpers.
//
// TODO: Use a TargetExt-type once some LLVM prerequisites are upstream.

include "llvm-dialects/Dialect/Dialect.td"

def LgcRtqDialect : Dialect {
  let name = "lgc.rtq";
  let cppNamespace = "lgc::rtq";
}

def V2F32 : TgConstant<(FixedVectorType F32, 2)>, Type;
def V3F32 : TgConstant<(FixedVectorType F32, 3)>, Type;
def A3V3F32 : TgConstant<(ArrayType V3F32, 3)>, Type;
def A4V3F32 : TgConstant<(ArrayType V3F32, 4)>, Type;
def PrivatePointer : TgConstant<(PointerType 5)>, Type;

class LgcRtqOp<string mnemonic_, list<Trait> traits_ = []>
    : Op<LgcRtqDialect, mnemonic_, traits_ # [NoUnwind, WillReturn]>;

// =====================================================================================================================
def InitializeOp :  LgcRtqOp<"initialize", [Memory<[(readwrite ArgMem)]>]> {
  let arguments =
      (ins PointerType:$ray_query, I64:$acceleration_structure, I32:$ray_flags,
           I32:$instance_inclusion_mask, V3F32:$ray_origin, F32:$t_min, V3F32:$direction, F32:$t_max);
  let results = (outs);

  let summary = "initialize a ray query object for traversal";
  let description = [{
    Initialize a ray query object. Any previous traversal state is discarded.
  }];
}

// =====================================================================================================================
def TerminateOp : LgcRtqOp<"terminate", [Memory<[(readwrite ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs);

  let summary = "terminate the current traversal";
  let description = [{
    Behavior is undefined if there is no traversal in progress, e.g. because the previous traversal has completed.
  }];
}

// =====================================================================================================================
def ProceedOp : LgcRtqOp<"proceed", [Memory<[(readwrite ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs I1:$traversal_incomplete);

  let summary = "proceed with the current traversal";
  let description = [{
    Proceed with the current traversal until a candidate intersection is found or the traversal is complete.

    Returns true if a candidate intersection was found and false if the traversal is complete.

    Behavior is undefined if no traversal is in progress, e.g. because the previous traversal has completed.
  }];
}

// =====================================================================================================================
def IntersectionCommitAabbOp : LgcRtqOp<"intersection.commit.aabb", [Memory<[(readwrite ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, F32:$t_hit);
  let results = (outs);

  let summary = "commit an AABB intersection";
  let description = [{
    Set the committed intersection to the current candidate AABB intersection with the given `t_hit` value.

    `t_hit` must satisfy `t_min <= t_hit <= t_max`, where `t_max` is the T value of the current committed intersection,
    if any, or the initial `t_max` value passed into `initialize` otherwise.

    The most recent use of `proceed` on the ray query object must have returned true, and the current candidate must
    be of AABB type, otherwise behavior is undefined.
  }];
}

// =====================================================================================================================
def IntersectionCommitTriangleOp : LgcRtqOp<"intersection.commit.triangle", [Memory<[(readwrite ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs);

  let summary = "commit the current candidate triangle intersection";
  let description = [{
    Sets the committed intersection to the current candidate triangle intersection.

    The most recent use of `proceed` on the ray query object must have returned true, and the current candidate must
    be of triangle type, otherwise behavior is undefined.
  }];
}

// =====================================================================================================================
def IntersectionTypeOp : LgcRtqOp<"intersection.type", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the type of the current candidate intersection";
  let description = [{
    Returns the type of a currently recorded intersection, depending on `committed`:

    - if `committed` is false, returns 0 for candidate triangles and 1 for candidate AABB
    - if `committed` is true, returns 0 for none, 1 for committed triangles, and 2 for committed AABB

    If `committed` is false, the most recent use of `proceed` must have returned true, or behavior is undefined.
  }];
}

// =====================================================================================================================
def RayTMinOp : LgcRtqOp<"ray.tmin", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs F32:$result);

  let summary = "return the Tmin value";
}

// =====================================================================================================================
def RayFlagsOp : LgcRtqOp<"ray.flags", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs I32:$result);

  let summary = "return the ray flags";
}

// =====================================================================================================================
def IntersectionTOp : LgcRtqOp<"intersection.t", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs F32:$result);

  let summary = "return the T value of an intersection";
  let description = [{
    Returns the T value of a currently recorded intersection, depending on `committed`.

    If `committed` is false and the most recent use of `proceed` returned false or the current candidate is not of
    triangle type, behavior is undefined.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionInstanceIdOp : LgcRtqOp<"intersection.instance.id", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the instance ID of an intersection";
  let description = [{
    Returns the instance ID of a currently recorded intersection, depending on `committed`.

    The instance ID is the user-provided instance ID on the bottom-level acceleration structure instance within the
    top-level structure.

    If `committed` is false and the most recent use of `proceed` returned false, behavior is undefined.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionInstanceIndexOp : LgcRtqOp<"intersection.instance.index", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the instance index of an intersection";
  let description = [{
    Returns the instance index of a currently recorded intersection, depending on `committed`.

    The instance index is the autogenerated index of the current instance in the top-level structure.

    If `committed` is false and the most recent use of `proceed` returned false, behavior is undefined.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionContributionToHitGroupIndexOp : LgcRtqOp<"intersection.contribution.to.hit.group.index",
                                                         [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the contribution to hit group index of an intersection";
  let description = [{
    Returns the contribution to hit group index of a currently recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionGeometryIndexOp : LgcRtqOp<"intersection.geometry.index", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the geometry index of an intersection";
  let description = [{
    Returns the geometry index of a currently recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionPrimitiveIndexOp : LgcRtqOp<"intersection.primitive.index", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I32:$result);

  let summary = "return the primitive index of an intersection";
  let description = [{
    Returns the primitive index of a currently recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionBarycentricsOp : LgcRtqOp<"intersection.barycentrics", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs V2F32:$result);

  let summary = "return the barycentrics of a triangle intersection";
  let description = [{
    Returns the barycentrics of a currently recorded triangle intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false or the current
    candidate is not of triangle type.

    If `committed` is true, behavior is undefined if there is no currently committed triangle intersection.
  }];
}

// =====================================================================================================================
def IntersectionFrontFaceOp : LgcRtqOp<"intersection.front.face", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs I1:$result);

  let summary = "return whether a triangle intersection hit the front face";
  let description = [{
    Returns whether a currently recorded triangle intersection hit the front face, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false or the current
    candidate is not of triangle type.

    If `committed` is true, behavior is undefined if there is no currently committed triangle intersection.
  }];
}

// =====================================================================================================================
def IntersectionCandidateAabbOpaqueOp : LgcRtqOp<"intersection.candidate.aabb.opaque", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs I1:$result);

  let summary = "return whether the current candidate AABB is opaque";
  let description = [{
    Returns whether the current candidate AABB is opaque.

    Note: SPIR-V does not explicitly allow UB here regardless of the ray query object state, so we don't either.
  }];
}

// =====================================================================================================================
def IntersectionObjectRayDirectionOp : LgcRtqOp<"intersection.object.ray.direction", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs V3F32:$result);

  let summary = "return the ray direction in object space";
  let description = [{
    Returns the ray direction in object space for a recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionObjectRayOriginOp : LgcRtqOp<"intersection.object.ray.origin", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs V3F32:$result);

  let summary = "return the ray origin in object space";
  let description = [{
    Returns the ray origin in object space for a recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionWorldRayDirectionOp : LgcRtqOp<"intersection.world.ray.direction", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs V3F32:$result);

  let summary = "return the ray direction in world space";
  let description = [{
    Returns the ray direction in world space.
  }];
}

// =====================================================================================================================
def IntersectionWorldRayOriginOp : LgcRtqOp<"intersection.world.ray.origin", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query);
  let results = (outs V3F32:$result);

  let summary = "return the ray origin in world space";
  let description = [{
    Returns the ray origin in world space.
  }];
}

// =====================================================================================================================
def IntersectionObjectToWorldOp : LgcRtqOp<"intersection.object.to.world", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs A4V3F32:$result);

  let summary = "return the object-to-world transformation matrix";
  let description = [{
    Returns the object-to-world transformation matrix for a recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

// =====================================================================================================================
def IntersectionWorldToObjectOp : LgcRtqOp<"intersection.world.to.object", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs A4V3F32:$result);

  let summary = "return the world-to-object transformation matrix";
  let description = [{
    Returns the world-to-object transformation matrix for a recorded intersection, depending on `committed`.

    If `committed` is false, behavior is undefined if the most recent use of `proceed` returned false.

    If `committed` is true, behavior is undefined if there is no currently committed intersection.
  }];
}

def GepOpaqueOp: LgcRtqOp<"gep.opaque", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins type:$base_type, AttrI1:$inbound, PointerType:$base_pointer, varargs:$offsets);
  let results = (outs PrivatePointer:$result);

  let summary = "GEP a rayquery pointer from a base object pointer";
  let description = [{
    Returns a opaque rayquery pointer from a base object pointer through gep array of indices
  }];
}

// =====================================================================================================================
def IntersectionTriangleVertexPositionsOp : LgcRtqOp<"intersection.triangle.vertex.position", [Memory<[(read ArgMem)]>]> {
  let arguments = (ins PointerType:$ray_query, AttrI1:$committed);
  let results = (outs A3V3F32:$result);

  let summary = "Fetch the triangle vertex points";
  let description = [{
    Returns the intersected vec3[3] triangle vertices from the ray query.
  }];
}
