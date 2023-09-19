/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

//===- PayloadAccessQualifiers.h - PAQ header -----------------------------===//
//
// Declare types and functions for the payload access qualifier support
// in DXR ray tracing, in particular computing serialization formats.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_PAYLOADACCESSQUALIFIERS_H
#define LLVM_TRANSFORMS_PAYLOADACCESSQUALIFIERS_H

#include "continuations/ContinuationsUtil.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/EnumeratedArray.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include <optional>

namespace llvm {

////////////////////////////////////////////////////////////////////////////////
// General
//
// PAQs allow to reduce register usage for the ray tracing payload by
// restricting the access to payload fields in shader stages.
//
// If a field is qualified as write(closesthit) : read(caller), then no
// registers need to be used before ClosestHit.
//
// The implementation consists of several steps:
//
//  - Importing DXIL PAQ data
//    Because fields in a payload can be nested structs that may in turn be
//    payload types, there are both explicitly PAQ-qualified fields and non
//    PAQ-qualified fields. For the latter, the PAQ data needs to be obtained
//    from the nested payload type. We construct a tree corresponding to the
//    (recursively contained) fields in the payload. Nodes may have PAQ data,
//    leaves correspond to non-struct fields (or empty structs) and must have
//    PAQ data (unless for empty payload structs). See PAQNode.
//
//  - Serialization layouts
//    For every payload copy into or out of a shader (e.g. CallerOut, AnyHitIn,
//    etc.), we define a payload serialization layout. For every field of the
//    payload that is live in the layout, the storage of that field is defined.
//    Currently, serialization layouts are i32 arrays wrapped into structs, and
//    the storage of a field is defined by a set of indices into that array. The
//    outer struct is not necessary, but improves readability of resulting IR
//    because whenever a particular layout is used, the name of the layout
//    struct appears. Fields are typically live in multiple layouts, and the
//    storage of the field must be the same in all these layouts, allowing to
//    store a field using one layout, and importing it using a different layout.
//    In some cases, this requirement leads to holes of unused indices in a
//    layout. Note that all data residing in payload registers is included in
//    serialization structs. This includes:
//     - Hit attribute storage (for TraceRay)
//       Hit attributes are live in AnyHit* and ClosestHitIn layouts. Because
//       hit attributes need to be accessed from Intersection shaders in some
//       cases that do not have access to payload types, we use fixed registers
//       (i.e. fixed indices) for hit attributes.
//     - Payload memory pointer (if too large for payload registers, both for
//       TraceRay and CallShader), using the first register/index.
//    See PAQSerializationLayoutKind for the different layouts.
//
//  - Performing imports/exports in shaders
//    In DXR, for each ray there is a single payload attached to that ray.
//    Shaders do not directly operate on the ray payload (also called global
//    payload), but instead allocate a local payload, copy data from the global
//    payload to the local payload, operate on the local payload, and upon
//    completion copy data from the local payload to the global payload.
//    With PAQ qualifiers, we copy only parts of the payload in the above.
//
// Further notes:
//  - PAQs do not apply to callable shaders. Actually, in the DXR spec, these
//    do not operate on payloads, but "parameters". Payloads may be used as
//    such. In the continuations pipeline, we refer to CallShader parameters as
//    payloads, and treat them similarly, by storing them in registers reserved
//    for the payload. Before/after CallShader as well as in a callable shader,
//    we need to import/export the whole payload, independent of PAQs. Despite
//    always having to copy all payload fields, we still use the PAQ
//    infrastructure with its serialization structs. This allows a consistent
//    implementation with less special case handling for CallShader.
//
//  - There might be parameters that are live but are neither read nor written,
//    for example read(caller) : write(caller) during a ClosestHit shader.
//    These need to be preserved, and DXR guarantees that the field is
//    accessible and writable in the local copy during the ClosestHit stage, but
//    that the global value is preserved. If we recurse into TraceRay() in the
//    shader, possibly overriding global register storage, we need to explicitly
//    caller-save a field if:
//     - the field is live
//     - the field is not written in the shader
//     - the field resides in payload registers (and not continuation memory)
//    Note that this also includes the payload memory pointer.
//    This is different to continuation state, which is callee-saved.
//
////////////////////////////////////////////////////////////////////////////////
// Payload layout strategy
//
// Through different shader stages, we may only grow or reduce the serialized
// payload, but never *change* the layout, that is, fields that are live need to
// stay at the same offset. This is because we do not know upfront which shader
// stage will import the payload that we export at some point in time.
// Considering all corner cases (e.g., caller->caller without any shader
// invocation), we may only use a single offset layout.
// In this process, we might import fields that have never been written because
// the stages did not run. It this case, we copy an undefined value to the
// local payload, and it is up to the client code to ensure to not access such
// fields in the local payload.
//
// First, we import the DXIL PAQs:
// For every payload type, we construct a tree structure to store information on
// its (nested) elements, see PAQNode.
//
// For all leaves of the tree (corresponding to non-struct fields), we know the
// access qualifiers, see PAQAccessMask. Based on that access mask, we determine
// the "lifetime class" of a node, for example Caller_To_ClosestHit or
// AnyHit_To_Caller, see PAQLifetimeClass. Fields in the same lifetime class
// have the same lifetime in the global payload. Lifetime classes are
// essentially obtained by the outmost write() and read() accesses. Lifetime
// classes are only used to decide which layouts a field is part of. For the
// copies between local and global payload, we still use the original access
// masks.
// If all children of an inner node (i.e. a struct type) have the same lifetime
// class, we propagate that lifetime class to the node.
//
// Then, we collect a representative list L of nodes of the tree such that:
//  - Every non-empty (nested) field of the payload is uniquely represented,
//  so:
//    - For every leaf N of non-struct type, exactly one node on the
//      path from the root to N is selected.
//    - Leaves of struct type represent empty-struct-fields and are ignored.
//  - Every selected node has a lifetime class
// The selected nodes determine the fields that are included in the
// serialization layouts. There is some freedom of choice here if there are
// inner nodes with lifetime classes, corresponding to fields of struct type
// S with a uniform lifetime class. For these, we could either add a single
// field of type S to serialization structs, or add its individual elements.
//
// To compute the layouts, we determine an order (PAQLifetimeClassPackingOrder)
// O of lifetime classes, which is currently a static, fixed order. Then, sort
// the elements of L by lifetime classes (according to O), and greedily allocate
// indices to fields. During this process, maintain a table that for every
// combination of layout and array index whether the index is already in use for
// some field in that layout.
// For details, see class LayoutComputer.
//
// The result is returned as PAQTraceRaySerializationInfo.
// For CallShader calls, we also compute a trivial serialization layout
// containing all payload fields (and possibly a payload mem pointer), and store
// it in a PAQCallShaderSerializationInfo containing a single
// PAQSerializationLayout.
//
// As an optimization, we let lifetime classes ClosestHit_To_Caller and
// Miss_To_Caller share storage, because these can never be live at the same
// time. When importing the data in the caller, we then need to import both
// from overlapping storage, leaving one of those in undefined state, and
// leave it up to the caller to ensure only defined data is accessed,
// depending on which shaders were invoked.
//
// The payload serialization structs include storage for the payload memory
// pointer if one of the layouts through the payload's lifetime is too large
// to fit into payload registers. Because we do not know that upfront, we
// first try to construct a layout without a payload memory pointer, and
// repeat with storage for the pointer if the computed layouts were too
// large.
//
// Also, the AnyHit and ClosestHit serialization layouts provide storage for
// hit attributes. We always provide layouts with enough storage for the
// maximum allowed hit attribute size. These are required to decide whether
// a payload memory pointer is needed. For the actually used layouts in
// AnyHit and ClosestHit, we create specialized layouts with the exact
// required amount of hit attribute storage if possible, i.e., if hit
// attribute storage is at the end of the struct.
// Currently, we always reserve registers 1 to 6 for hit attributes,
// so any nontrivial payload needs registers beyond that (starting with register
// 7) and does not profit from specialization for a reduced hit attribute size.
// In the future, we might exploit analyses of whole pipelines to reduce hit
// attribute storage.
//
// The currently used fixed packing order of lifetime classes needs to
// perform some trade-offs of where dummy fields are needed. For example,
// AnyHit_To_ClosestHitAndMiss and AnyHit_To_ClosestHit are live in
// CallerOut to ensure stable offsets of Caller_To_AnyHit. We could move
// Caller_To_AnyHit to before these, but then Caller_To_AnyHit would need to
// be live in ClosestHitIn. We tried to minimize the ClosestHitIn state,
// which may help potential repacking before ClosestHitIn. When returning
// from AnyHit, indicating to accept the hit and continue searching, we use
// the full attribute size, if there are live fields behind (in
// Caller_To_AnyHit or AnyHit_To_AnyHit).
//
////////////////////////////////////////////////////////////////////////////////
// Current layout visualization
//
// A lifetime class L1 leads to a hole in a layout because it is allocated
// before some other lifetime class L2 that shares lifetime with L1, where L2 is
// live in the layout but L1 is not. In that case, L1 is marked as dummy below,
// to indicate that its storage is occupied and shifts all following lifetime
// classes backwards. Also note that hit attribute storage is re-used for new
// lifetime classes added to layouts in ClosestHitOut, and MissOut (e.g.
// ClosestHit_To_Caller), and only after having completely used hit attribute
// storage, these added lifetime classes consume storage as indicated below.
// As of now, the layouts of serialization are as follows:
// - CallerOut:
//  - Caller_To_Caller
//  - AnyHit_To_Caller                            (dummy, if following nonempty)
//  - Caller_To_ClosestHitAndMiss
//  - Caller_To_ClosestHit
//  - AnyHit_To_ClosestHitAndMiss                 (dummy, if following nonempty)
//  - AnyHit_To_ClosestHit                        (dummy, if following nonempty)
//  - Caller_To_AnyHit
// - AnyHitIn:                                 (also used as AnyHitOutIgnoreHit)
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - Caller_To_ClosestHitAndMiss
//  - Caller_To_ClosestHit
//  - AnyHit_To_ClosestHitAndMiss
//  - AnyHit_To_ClosestHit
//  - Caller_To_AnyHit
//  - AnyHit_To_AnyHit
// - AnyHitOutAcceptHit:
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - Caller_To_ClosestHitAndMiss
//  - Caller_To_ClosestHit
//  - AnyHit_To_ClosestHitAndMiss
//  - AnyHit_To_ClosestHit
//  - Caller_To_AnyHit
//  - AnyHit_To_AnyHit
// - ClosestHitIn:                 (also used as AnyHitOutAcceptHitAndEndSearch)
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - Caller_To_ClosestHitAndMiss
//  - Caller_To_ClosestHit
//  - AnyHit_To_ClosestHitAndMiss
//  - AnyHit_To_ClosestHit
// - ClosestHitOut
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - ClosestHitAndMiss_To_Caller,
//  - ClosestHit_To_Caller,
// - MissIn:
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - Caller_To_ClosestHitAndMiss
//  - Caller_To_ClosestHit                        (dummy, if following nonempty)
//  - AnyHit_To_ClosestHitAndMiss
// - MissOut:
//  - Caller_To_Caller
//  - AnyHit_To_Caller
//  - ClosestHitAndMiss_To_Caller,
//  - Miss_To_Caller
//
// IDEA: Reduce incoming serialization sizes for cases of write() without
//       read() of live value:
//       For AnyHit, ClosestHit and Miss, if a field is live in the incoming
//       layout and written but not read, then it can be removed from the
//       input layout, given other offsets do not change.
//       This would require to 1) detect such cases and remove them from the
//       serialization suffix, and 2) moving these to the end of the struct
//       to have 1) succeed as often as possible.
//
// IDEA: Improve serialization packing:
//       There are two basic ways to improve serialization layout packings:
//        - Dynamically compute a packing order of lifetime classes
//          A first easy improvement could be to use a fixed candidate list
//          of orderings, and choose the best one.
//
// IDEA: Eliminate the payload mem pointer in MissOut and ClosestHitOut layouts:
//       The caller can reconstruct the payload memory pointer from the
//       continuation stack pointer, similar to how we find the continuation
//       state. This is because when returning to the caller, the payload
//       storage and continuation state is on top of the stack. This would allow
//       to use the free payload mem register in MissOut and ClosestHitOut for
//       fields with one of Miss_To_Caller, ClosestHit_To_Caller or
//       ClosestHitAndMiss_To_Caller lifetime class.
//
// IDEA: Split Caller_To_ClosestHitAndMiss into Caller_To_ClosestHitAndMiss and
//       Caller_To_Miss. This would allow to remove Caller_To_Miss from
//       ClosestHitIn if not needed as dummy (in particular requiring
//       Caller_To_ClosestHit to be empty). Packing order would be
//       Caller_To_ClosestHitAndMiss, Caller_To_Miss, Caller_To_ClosestHit.
//       The same applies to AnyHit_To_ClosestHitAndMiss.
//
////////////////////////////////////////////////////////////////////////////////
// Copying between global and local payload
//
// Serialization layouts (PAQSerializationLayout) provide a serialization
// struct type, and a map of PAQNodes to indices of elements of the struct
// type. To copy the payload, we recursively traverse the PAQ tree of the
// payload, starting with its root node. When we encounter a node contained
// in the map, we check the node's access mask to decide whether the copy
// should be performed, and do so if required.
//
// Hit attributes and the payload memory pointer are copied separately.
//
////////////////////////////////////////////////////////////////////////////////

// Stages relevant for PAQ, with respect to the payload attached to a ray.
// Does not apply to CallShader calls.
enum class PAQShaderStage {
  // The caller of TraceRay (possibly in ClosestHit or Miss)
  Caller = 0,
  // The following three stages apply when entering and leaving the
  // corresponding shader stages.
  AnyHit,
  ClosestHit,
  Miss,

  Count
};

// Prints enum value in lower case (as in HLSL)
llvm::raw_ostream &operator<<(llvm::raw_ostream &, PAQShaderStage);

// List of all valid PAQShaderStage values
constexpr std::array<PAQShaderStage,
                     static_cast<std::size_t>(PAQShaderStage::Count)>
    PAQShaderStages = {PAQShaderStage::Caller, PAQShaderStage::AnyHit,
                       PAQShaderStage::ClosestHit, PAQShaderStage::Miss};

// Prints enum value in lower case (as in HLSL)
enum class PAQAccessKind { Read = 0, Write, NumKinds };
llvm::raw_ostream &operator<<(llvm::raw_ostream &, PAQAccessKind);

// Access qualification of a payload field.
// Corresponds to a single line of PAQ qualifiers, e.g.
//    read(caller,anyhit) : write(anyhit)
// Essentially a convenience wrapper around a bitmask.
class PAQAccessMask {
public:
  constexpr bool get(PAQShaderStage Stage, PAQAccessKind AccessKind) const {
    return AccessMask & getBitmask(Stage, AccessKind);
  }

  constexpr PAQAccessMask &set(PAQShaderStage Stage, PAQAccessKind AccessKind,
                               bool Value = true) {
    if (Value) {
      AccessMask |= getBitmask(Stage, AccessKind);
    } else {
      AccessMask &= ~getBitmask(Stage, AccessKind);
    }
    return *this;
  }

  bool operator==(const PAQAccessMask &RHS) const {
    return AccessMask == RHS.AccessMask;
  }

  // Prints HLSL-like qualifier string as in "write(..) : read(..)"
  // If AccessKind is set, only prints the part corresponding to that kind.
  void print(llvm::raw_ostream &,
             std::optional<PAQAccessKind> AccessKind = {}) const;

  bool empty() const { return AccessMask == 0u; }

private:
  // Offset of the bit corresponding to (Stage, AccessKind) in AccessMask
  static constexpr uint32_t getBitmask(PAQShaderStage Stage,
                                       PAQAccessKind AccessKind) {
    uint32_t Offset = static_cast<uint32_t>(Stage) *
                          static_cast<uint32_t>(PAQAccessKind::NumKinds) +
                      static_cast<uint32_t>(AccessKind);
    return 1u << Offset;
  }

  uint32_t AccessMask = 0u;

  static_assert(sizeof(AccessMask) * CHAR_BIT >=
                    static_cast<uint32_t>(PAQShaderStage::Count) *
                        static_cast<uint32_t>(PAQAccessKind::NumKinds),
                "Increase width of AccessMask!");
};

inline raw_ostream &operator<<(raw_ostream &Stream,
                               const PAQAccessMask &AccessMask) {
  AccessMask.print(Stream);
  return Stream;
}

// Lifetime of a payload field.
enum class PAQLifetimeClass : uint32_t {
  // Always live (written in Caller, read in Caller)
  Caller_To_Caller = 0,
  AnyHit_To_Caller,
  // Written in Caller, read in Miss and possibly ClosestHit
  Caller_To_ClosestHitAndMiss,
  Caller_To_ClosestHit,
  // Written in AnyHit, read in Miss and possibly ClosestHit
  AnyHit_To_ClosestHitAndMiss,
  // Includes hit attribute storage
  AnyHit_To_ClosestHit,
  Caller_To_AnyHit,
  AnyHit_To_AnyHit,
  // Written in both ClosestHit and Miss
  ClosestHitAndMiss_To_Caller,
  // Written only in ClosestHit
  ClosestHit_To_Caller,
  // Written only in Miss
  Miss_To_Caller,

  Last = Miss_To_Caller,
  Count
};

constexpr std::array<PAQLifetimeClass,
                     static_cast<std::size_t>(PAQLifetimeClass::Count)>
    PAQLifetimeClasses = {PAQLifetimeClass::Caller_To_Caller,
                          PAQLifetimeClass::AnyHit_To_Caller,
                          PAQLifetimeClass::Caller_To_ClosestHitAndMiss,
                          PAQLifetimeClass::Caller_To_ClosestHit,
                          PAQLifetimeClass::AnyHit_To_ClosestHitAndMiss,
                          PAQLifetimeClass::AnyHit_To_ClosestHit,
                          PAQLifetimeClass::Caller_To_AnyHit,
                          PAQLifetimeClass::AnyHit_To_AnyHit,
                          PAQLifetimeClass::ClosestHitAndMiss_To_Caller,
                          PAQLifetimeClass::ClosestHit_To_Caller,
                          PAQLifetimeClass::Miss_To_Caller};

llvm::raw_ostream &operator<<(llvm::raw_ostream &, PAQLifetimeClass);

enum class PAQSerializationLayoutKind {
  CallerOut = 0,
  AnyHitIn,
  // Includes maximum possible hit attribute storage, because we do not know the
  // size of the currently committed hit attributes
  AnyHitOutIgnoreHit = AnyHitIn,
  // Separate because if the hit is accepted, we override the committed hit
  // attriubutes with a known size.
  AnyHitOutAcceptHit,
  // Separate so lifetimes ending in AnyHit can be omitted
  AnyHitOutAcceptHitAndEndSearch,
  ClosestHitIn = AnyHitOutAcceptHitAndEndSearch,
  MissIn,
  ClosestHitOut,
  MissOut,

  Last = MissOut,
  Count
};

constexpr std::array<PAQSerializationLayoutKind,
                     static_cast<std::size_t>(
                         PAQSerializationLayoutKind::Count)>
    PAQSerializationLayoutKinds = {
        PAQSerializationLayoutKind::CallerOut,
        PAQSerializationLayoutKind::AnyHitIn,
        PAQSerializationLayoutKind::AnyHitOutAcceptHit,
        PAQSerializationLayoutKind::AnyHitOutAcceptHitAndEndSearch,
        PAQSerializationLayoutKind::MissIn,
        PAQSerializationLayoutKind::ClosestHitOut,
        PAQSerializationLayoutKind::MissOut};

llvm::raw_ostream &operator<<(llvm::raw_ostream &, PAQSerializationLayoutKind);

// Status of a field (or its lifetime class) in a serialization layout.
enum class PAQLivenessStatus : uint8_t {
  Live, // Is live, included in serialization struct
  Dead, // Is dead, not included in serialization struct
  Dummy // Is functionally dead, but there are following lifetime classes in the
        // layout that are still live. If these are non-empty, then the Dummy
        // field's storage is still live to ensure the following fields have
        // stable offsets in the serialization storage.
};

// Stores the PAQLivenessStatus for each combination of liveness status and
// layout kind.
// This can be computed more efficiently for all combinations together, hence
// using a lookup table instead of querying each individual combination.
using PAQLivenessStatusTable = llvm::EnumeratedArray<
    llvm::EnumeratedArray<PAQLivenessStatus, PAQSerializationLayoutKind,
                          PAQSerializationLayoutKind::Last, std::size_t>,
    PAQLifetimeClass, PAQLifetimeClass::Last, std::size_t>;

// A permutation of all PAQLifetimeClass values.
//
// Specifies the relative order of lifetime classes in the serialization
// layout. The relative position P of a lifetime class C in the layout is
// determined as follows: Let C' be a lifetime class that precedes C in the
// ordering which is live together with C in at least one LayoutKind, and
// which has maximum position P' among these. Then P = P' + 1. If C' does not
// exist, then P = 0 (for Caller_To_Caller). To obtain the actual
// serialization structs, the live (or dummy) elements are sorted by the
// position of their lifetime class in this ordering. Changing this ordering
// can change which layouts contain which dummy fields, and be used to
// trade-off payload serialization size at one stage vs at a different stage.
// However, some lifetime classes dominate others (i.e. are always live if the
// other is live), and hence should always precede the dominated ones in the
// ordering to avoid unnecessary dummy fields. In other words, the order should
// be a topological order of the lifetime domination graph. For example,
// Caller_To_Caller should always come first.
using PAQLifetimeClassPackingOrder =
    std::array<PAQLifetimeClass,
               static_cast<std::size_t>(PAQLifetimeClass::Count)>;

// Determine an ordering of lifetime classes in the TraceRay serialization
// layout. Currently, we use a fixed hardcoded order, but we could dynamically
// compute a better one that reduces dummy fields.
PAQLifetimeClassPackingOrder determineLifetimeClassPackingOrder();

// We compute the liveness status table dynamically at runtime. As long as we
// use a static ordering, this could be done manually in a large switch
// statement, and was done so in the past, but that was a huge, error-prone case
// distinction.
PAQLivenessStatusTable
computeLivenessStatusTable(const PAQLifetimeClassPackingOrder &Ordering);

// Try to determine the unique layout kind for the given shader stage and access
// kind. If there are multiple relevant layouts, returns std::nullopt; these
// cases must be handled manually:
//  - read(caller): There is no unique layout kind, because we import
//                  multiple layouts (ClosestHitOut, MissOut).
//  - write(anyhit): There are multiple possible layout kinds.
std::optional<PAQSerializationLayoutKind>
tryDetermineLayoutKind(PAQShaderStage ShaderStage, PAQAccessKind AccessKind);

// For every payload struct, we store PAQ qualifiers of its possibly nested
// fields in a tree whose structure corresponds to the nested fields structure
// of the payload struct.
// This class represents nodes of this tree.
//
// Every node represents either the payload itself (for the root node),
// or a field in its parent node.
// Fields of struct type have separate child nodes for each of their elements,
// even if elements share the same type.
//
// Arrays or vectors are not dissolved and kept as leaf nodes.
//
// All data to be included in payload serialization structs is represented by
// PAQNodes which are then added to these structs. This means that there are
// also PAQNodes for the optional payload mem pointer, and hit attribute
// storage. Payload serialization formats map PAQNode* pointers to element
// indices of serialization structs, see PAQSerializationLayout. Thus, we must
// ensure stable addresses of such PAQNodes, e.g. using unique_ptr.
struct PAQNode {
  // The type this node refers to. For the root node, this is the tree's payload
  // struct type, otherwise the type of the field (in the parent's struct)
  // represented by this node.
  Type *Ty = nullptr;

  // If Ty is a struct type, store nodes for all elements, in the same order.
  // Is empty for empty structs.
  std::vector<PAQNode> Children = {};

  // Access mask imported from DXIL, and propagated downwards for nested types.
  //  - If Ty is not a struct type, AccessMask must be set.
  //  - If Ty is a non-payload struct type, AccessMask must be set.
  //  - If Ty is a payload struct type, AccessMask may be unset. This is only
  //  the case if all parent nodes' types are also payload structs, in which
  //  case access qualifiers are used from the nested fields.
  //
  // If the access mask is set, it is the same for all nodes in this subtree.
  //
  // Note that input HLSL might contain "inconsistent" qualifiers: If a payload
  // struct OuterPayload contains a field of type NonPayload that in turn
  // contains a field of type InnerPayload, then the NonPayload field in the
  // OuterPayload has PAQ qualifiers, but the fields of InnerPayload are
  // qualified as well. DXC does not require these qualifiers to agree.
  // In this case, the outer qualifiers of the NonPayload field inside
  // OuterPayload win. In our representation, the node corresponding to the
  // field of type InnerPayload in OuterPayload will have a set AccessMask, even
  // if it a struct of payload type.
  //
  // Also note that empty access masks (write() : read()) are allowed in HLSL.
  // Nodes for such fields contain a set but empty AccessMask, but no
  // LifetimeClass.
  std::optional<PAQAccessMask> AccessMask = {};

  // Determines the interval during which the field is live.
  // See documentation of PAQLifetimeClass.
  // There are two cases in which LifetimeClass is set:
  //  - The node is a leaf of the tree, Ty is not an empty struct type, and
  //    AccessMask is set. Then, LifetimeClass is derived from AccessMask.
  //  - The node is an inner node, and all children have the same lifetime
  //    class. In this case the inner node is assigned the same lifetime class.
  //    This allows to keep the fields of nested structs of uniform
  //    LifetimeClass together if we want to. Note that this does not require
  //    uniform access masks of children.
  std::optional<PAQLifetimeClass> LifetimeClass = {};

  // Prints the subtree rooted at this node in a recursive fashion.
  // Prints a single line per node, indented by the depth.
  void print(llvm::raw_ostream &) const;

  // Collect a set of PAQNodes representing the tree rooted at this node,
  // and append it to Result.
  void collectLeafNodes(SmallVectorImpl<const PAQNode *> &Result) const;
};

inline raw_ostream &operator<<(raw_ostream &Stream, const PAQNode &NodeInfo) {
  NodeInfo.print(Stream);
  return Stream;
}

//////////////////////////////////////////////////////////////////////////////
// Serialization layout types and computation

// Identifies all necessary parameters to account for when creating a payload
// serialization layout for TraceRay or Callshader.
struct PAQPayloadConfig {
  // Prefer explicit constructor over aggregate initialization to catch cases
  // of missing fields in cases we add fields to this struct.
  PAQPayloadConfig(Type *PayloadType, uint32_t MaxHitAttributeByteCnt)
      : PayloadTy{PayloadType}, MaxHitAttributeByteCount{
                                    MaxHitAttributeByteCnt} {}
  Type *PayloadTy = nullptr;
  // Only relevant for TraceRay:
  uint32_t MaxHitAttributeByteCount = 0;

  friend bool operator==(const PAQPayloadConfig &LHS,
                         const PAQPayloadConfig &RHS) {
    return std::tie(LHS.PayloadTy, LHS.MaxHitAttributeByteCount) ==
           std::tie(RHS.PayloadTy, RHS.MaxHitAttributeByteCount);
  }
};

template <> struct DenseMapInfo<PAQPayloadConfig> {
  using T = PAQPayloadConfig;

  static T getEmptyKey() { return T{DenseMapInfo<Type *>::getEmptyKey(), 0}; }
  static T getTombstoneKey() {
    return T{DenseMapInfo<Type *>::getTombstoneKey(), 0};
  }
  static unsigned getHashValue(const T &Val) {
    return llvm::hash_combine(Val.PayloadTy, Val.MaxHitAttributeByteCount);
  }
  static bool isEqual(const T &LHS, const T &RHS) { return LHS == RHS; }
};

// Half-open index interval, representing the indices in the I32 serialization
// array used to store a node.
struct PAQIndexInterval {
  // First index in the interval occupied by the node
  uint32_t Begin;
  // First index NOT included in the interval
  uint32_t End;

  uint32_t size() const { return End - Begin; }

  bool operator==(const PAQIndexInterval &Other) const {
    return Begin == Other.Begin && End == Other.End;
  }
  bool operator!=(const PAQIndexInterval &Other) const {
    return !(*this == Other);
  }
  // Sort lexicographically by (Begin, End)
  bool operator<(const PAQIndexInterval &Other) const {
    return std::tie(Begin, End) < std::tie(Other.Begin, Other.End);
  }
};

// Intervals of indices in the I32 serialization array used for this node
// Typically is just a single interval, but due to nodes at fixed indices (hit
// attributes), we may need to split the storage of some nodes to avoid
// unused holes.
using PAQIndexIntervals = SmallVector<PAQIndexInterval, 2>;

// Stores data about the storage of a node in a serialization struct.
struct PAQNodeStorageInfo {
  PAQIndexIntervals IndexIntervals;

  void print(raw_ostream &O) const;
};

inline raw_ostream &operator<<(raw_ostream &O, const PAQNodeStorageInfo &Info) {
  Info.print(O);
  return O;
}

// Stores a particular serialization format for a given payload type and
// PAQSerializationLayoutKind
struct PAQSerializationLayout {
  // May be nullptr if no payload state is live
  StructType *SerializationTy = nullptr;

  // Maps nodes to indices of elements of SerializationTy.
  MapVector<const PAQNode *, PAQNodeStorageInfo> NodeStorageInfos;

  // PAQNode representing the root node of the PAQ tree of the payload struct
  const PAQNode *PayloadRootNode = nullptr;

  // PAQNode representing the mem pointer for payload storage, if necessary
  const PAQNode *PayloadMemPointerNode = nullptr;

  // PAQNode representing the hit attribute storage. Depending on the
  // stage/layout, this can be the maximum possible hit attribute storage, or
  // the actually needed one based on the used hit attribute type.
  const PAQNode *HitAttributeStorageNode = nullptr;

  // Number of I32s required to store SerializationTy
  uint32_t NumStorageI32s = 0;

  void print(raw_ostream &O, bool SingleLine = false) const;
  // Calls print() on dbgs()
  void dump() const;
};

enum class PAQSerializationInfoKind { TraceRay = 1, CallShader };

// Stores serialization info for a payload type for the whole lifetime of the
// payload for one of either a TraceRay or a CallShader invocation.
// PAQTraceRaySerializationInfo and PAQCallShaderSerializationInfo inherit from
// this.
struct PAQSerializationInfoBase {
  PAQSerializationInfoBase(PAQSerializationInfoKind InfoKind)
      : Kind{InfoKind} {}
  PAQSerializationInfoBase(PAQSerializationInfoBase &&) = default;
  PAQSerializationInfoBase &operator=(PAQSerializationInfoBase &&) = default;
  virtual ~PAQSerializationInfoBase() = default;

  // To implement classof(..) for LLVM casting.
  PAQSerializationInfoKind Kind;

  // Root node of the PAQ tree for the payload type and the invocation type.
  // For TraceRay, this tree contains PAQ qualifiers.
  // For CallShader, PAQ qualifiers are not set, but the nested struct hierarchy
  // is still represented by the PAQ tree.
  const PAQNode *PayloadRootNode = nullptr;

  // Node representing the mem pointer for the part of the payload
  // that did not fit into registers.
  // Access mask is write(caller) : read(all)
  // By using a PAQNode for this, it can be automatically included in the
  // serialization structs, and its element position can be obtained from
  // NodeIndices.
  // Is nullptr if no stack storage is required.
  std::unique_ptr<PAQNode> PayloadMemPointerNode;

  // Maximum required number of I32s for any of the included struct types.
  // When allocating space for the payload on the stack, we use this value
  // (minus payload registers), because later serialization structs need to be
  // stored to the same space.
  uint32_t MaxStorageI32s = 0;

  // Collect all nodes to be included in any represented serialization structs,
  // and appends them to Result. The nodes are in-order as in the struct,
  // with the mem pointer node coming first if present
  virtual void collectAllNodes(SmallVectorImpl<const PAQNode *> &Result) const {
    if (PayloadMemPointerNode) {
      Result.push_back(PayloadMemPointerNode.get());
    }
    PayloadRootNode->collectLeafNodes(Result);
  }
};

// Contains specialized layouts for known hit attribute sizes
struct PAQHitGroupLayoutInfo {
  // Compared to the default AnyHitOutAcceptHit layout, this can reduce the hit
  // attribute storage to the actually required amount IF it is the last field.
  // This is because if we are accepting the hit, any already committed hit data
  // becomes obsolete, and only the new hit data needs to be live.
  PAQSerializationLayout AnyHitOutAcceptHitLayout;

  // In ClosestHit, hit attribute storage is the last field.
  // Thus, we can trim it to the actually required size of the hit attributes.
  PAQSerializationLayout ClosestHitInLayout;

  // Number of I32s for hit attribute storage
  uint32_t NumHitAttributesI32s = 0;

  // PAQNode representing NumHitAttributesI32s many I32s, or nullptr if
  // NumHitAttributesI32s is zero. Depending on the other used live fields,
  // either this node or the worst-case node from PAQTraceRaySerializationInfo
  // is used in layouts.
  std::unique_ptr<PAQNode> HitAttributesNode;
};

// Stores complete serialization info for a particular payload type for the
// whole TraceRay pipeline.
struct PAQTraceRaySerializationInfo : public PAQSerializationInfoBase {
  PAQTraceRaySerializationInfo()
      : PAQSerializationInfoBase(PAQSerializationInfoKind::TraceRay) {}
  PAQTraceRaySerializationInfo(PAQTraceRaySerializationInfo &&) = default;
  PAQTraceRaySerializationInfo &
  operator=(PAQTraceRaySerializationInfo &&) = default;

  static bool classof(const PAQSerializationInfoBase *IB) {
    return IB->Kind == PAQSerializationInfoKind::TraceRay;
  }

  PAQPayloadConfig PAQConfig = {nullptr, 0};

  // Indexed by PAQSerializationLayoutKind
  // For AnyHit and ClosestHit, these reserve space for the worst-case hit
  // attribute size, required to compute the maximum required payload storage
  // size. AnyHit and ClosestHit shaders know the exact attribute type and size,
  // and use specialized layouts in SpecializedHitGroupLayouts.
  llvm::EnumeratedArray<PAQSerializationLayout, PAQSerializationLayoutKind,
                        PAQSerializationLayoutKind::Last, std::size_t>
      LayoutsByKind;

  // Specialized layouts for known attribute size
  // Indexed by number of required I32s for hit attribute storage,
  // strictly smaller than MaximumNumHitAttributesI32s
  // Populated on demand.
  SmallDenseMap<uint64_t, PAQHitGroupLayoutInfo, 8> SpecializedHitGroupLayouts;

  // Maximum possible number of I32s required for hit attribute storage.
  uint32_t MaximumNumHitAttributesI32s = 0;

  // Node representing the maximum possible required storage for hit attributes
  // that did not fit into system data. Access mask is write(anyhit) :
  // read(anyhit, closesthit).
  // AnyHit and ClosestHit shaders know the required size, and use the
  // layout from SpecializedHitGroupLayouts if below the maximum size, which
  // also contains a hit attribute PAQNode of known exact size.
  std::unique_ptr<PAQNode> WorstCaseHitAttributesNode;

  // Creates a serialization info for the given payload config for usage in a
  // TraceRay pipeline.
  // If RootNode is nullptr, a new PAQ tree for PayloadType is created without
  // any PAQ qualifiers, assuming write(all) + read(all). Otherwise, RootNode
  // must be the root node of a PAQ tree containing PAQ access qualifiers for
  // PayloadType. Ownership of RootNode is transferred to the returned object.
  static std::unique_ptr<PAQTraceRaySerializationInfo>
  create(Module &M, const PAQPayloadConfig &PAQConfig, const PAQNode &RootNode,
         uint64_t PayloadRegisterCount);

  virtual void
  collectAllNodes(SmallVectorImpl<const PAQNode *> &Result) const override {
    PAQSerializationInfoBase::collectAllNodes(Result);
    if (WorstCaseHitAttributesNode)
      Result.push_back(WorstCaseHitAttributesNode.get());
  }

  // Compute a PAQHitGroupLayoutInfo, containing specialized serialization
  // layouts for a fixed number of required I32s for hit attribute storage
  PAQHitGroupLayoutInfo
  createHitGroupLayoutInfo(Module &M, uint32_t PayloadHitAttrI32s) const;
};

// Serialization info for CallShader calls.
// PAQ access flags CallShader do not apply to CallShader, hence we always
// read/write all payload fields. This class allows a consistent implementation
// without special case handling for CallShader.
struct PAQCallShaderSerializationInfo : public PAQSerializationInfoBase {
  PAQCallShaderSerializationInfo()
      : PAQSerializationInfoBase(PAQSerializationInfoKind::CallShader) {}

  static bool classof(const PAQSerializationInfoBase *IB) {
    return IB->Kind == PAQSerializationInfoKind::CallShader;
  }

  PAQSerializationLayout CallShaderSerializationLayout;

  // Computes a serialization info for CallShader calls for the given payload
  // type. Note that CallShader calls are not affected by PAQ access qualifiers.
  static std::unique_ptr<PAQCallShaderSerializationInfo>
  create(Module &M, const PAQPayloadConfig &PAQConfig,
         const PAQNode &PAQRootNode, uint64_t PayloadRegisterCount);
};

// Helper class to obtain serialization infos, importing DXIL PAQ metadata,
// and caching already seen serialization infos.
class PAQSerializationInfoManager {
public:
  PAQSerializationInfoManager(Module *M, uint32_t MaxPayloadRegisterCount);
  PAQSerializationInfoManager(const PAQSerializationInfoManager &) = delete;
  PAQSerializationInfoManager(PAQSerializationInfoManager &&) = default;

  // Returns the result of either getOrCreateTraceRaySerializationInfo or
  // getOrCreateCallShaderSerializationInfo depending on ShaderKind.
  PAQSerializationInfoBase &
  getOrCreateSerializationInfo(const PAQPayloadConfig &PayloadConfig,
                               DXILShaderKind ShaderKind);

  // Check whether a serialization info for the given
  // payload type has already been computed (or imported from DXIL metadata).
  // If so, return the existing one.
  // Otherwise, compute a new serialization info with trivial qualifiers
  // (write+read everything).
  // Result is non-const to allow adding custom hitgroup layouts later on.
  PAQTraceRaySerializationInfo &
  getOrCreateTraceRaySerializationInfo(const PAQPayloadConfig &PAQConfig);

  // Same as above, but for CallShader.
  PAQCallShaderSerializationInfo &
  getOrCreateCallShaderSerializationInfo(const PAQPayloadConfig &PAQConfig);

  // For ClosestHit and AnyHitOutAcceptHit layouts, the layout depends on the
  // actually used hit attribute type. In this case, the HitAttributesTy
  // argument must be non-null. In all other cases, it is ignored.
  const PAQSerializationLayout &
  getOrCreateTraceRayLayout(PAQTraceRaySerializationInfo &TraceRayInfo,
                            PAQSerializationLayoutKind LayoutKind,
                            Type *HitAttributesTy = nullptr);

  // Convenience wrapper that selects the layout to be used for the payload
  // incoming to a shader on shader entry.
  const PAQSerializationLayout &getOrCreateShaderStartSerializationLayout(
      PAQSerializationInfoBase &SerializationInfo, DXILShaderKind ShaderKind,
      Type *HitAttributesTy = nullptr);
  // Convenience wrapper that selects the layout to be used for the payload
  // outgoing of a shader on shader exit.
  const PAQSerializationLayout &getOrCreateShaderExitSerializationLayout(
      PAQSerializationInfoBase &SerializationInfo, DXILShaderKind ShaderKind,
      Type *HitAttributesTy = nullptr,
      AnyHitExitKind AHExitKind = AnyHitExitKind::None);

  enum class MaxPayloadStorageConsideration : uint8_t {
    ConsiderOnlyTraceRay,
    ConsiderOnlyCallShader,
    ConsiderTraceRayAndCallShader
  };

  // Get the maximum number of I32s required to store a serialization of the
  // given payload type in the given function. For CallShader, this is just the
  // number of I32s required to store the CallShader serialization. For
  // TraceRay, this takes the maximum over all serialization formats.
  uint32_t getMaxPayloadStorageI32s(
      const PAQPayloadConfig &PAQConfig,
      MaxPayloadStorageConsideration Consideration =
          MaxPayloadStorageConsideration::ConsiderTraceRayAndCallShader);

  uint32_t
  getMaxPayloadStorageI32sForTraceRayFunc(const PAQPayloadConfig &PAQConfig) {
    return getMaxPayloadStorageI32s(
        PAQConfig, MaxPayloadStorageConsideration::ConsiderOnlyTraceRay);
  }

  uint32_t
  getMaxPayloadStorageI32sForCallShaderFunc(const PAQPayloadConfig &PAQConfig) {
    return getMaxPayloadStorageI32s(
        PAQConfig, MaxPayloadStorageConsideration::ConsiderOnlyCallShader);
  }

private:
  Module *Mod = {};
  uint32_t MaxPayloadRegisterCount = {};

  // Stores per-payload-type data
  template <typename SerializationInfoT> struct PAQCache {
    // For TraceRay payload types with PAQ metadata, these are imported upon
    // construction into PAQRootNodes. For remaining TraceRay payload types, and
    // all CallShader payload types, we construct trivial (i.e. always read and
    // write everything) PAQNodes on demand.
    MapVector<Type *, std::unique_ptr<PAQNode>> PAQRootNodes;
    MapVector<PAQPayloadConfig, std::unique_ptr<SerializationInfoT>>
        SerializationInfos;

    SerializationInfoT &
    getOrCreateSerializationInfo(Module &M, uint32_t MaxPayloadRegisterCount,
                                 const PAQPayloadConfig &PAQConfig);
  };

  PAQCache<PAQTraceRaySerializationInfo> TraceRayCache;
  PAQCache<PAQCallShaderSerializationInfo> CallShaderCache;
};

} // namespace llvm

#endif
