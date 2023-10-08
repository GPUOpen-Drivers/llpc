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

//===- PayloadAccessQualifiers.cpp - PAQ implementation -------------------===//
//
// This file implements support for payload access qualifiers in DXR raytracing:
// - Import access qualifiers from DXIL metadata
// - Compute payload serialization struct layouts
//
// The actual serialization structs are then used in the LowerRaytracingPipeline
// pass to copy between global and local payloads.
//
//===----------------------------------------------------------------------===//

#include "continuations/PayloadAccessQualifiers.h"
#include "continuations/Continuations.h"
#include "llvm/ADT/EnumeratedArray.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>

using namespace llvm;

// Maybe change to PAQ-specific value
#define DEBUG_TYPE "lower-raytracing-pipeline"

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Stream,
                                    PAQShaderStage ShaderStage) {
  StringRef String = [ShaderStage]() {
    switch (ShaderStage) {
    case PAQShaderStage::Caller:
      return "caller";
    case PAQShaderStage::ClosestHit:
      return "closesthit";
    case PAQShaderStage::Miss:
      return "miss";
    case PAQShaderStage::AnyHit:
      return "anyhit";
    case PAQShaderStage::Count:
      break;
    }
    llvm_unreachable("Unknown stage");
  }();
  Stream << String;
  return Stream;
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Stream,
                                    PAQAccessKind AccessKind) {
  if (AccessKind == PAQAccessKind::Read) {
    Stream << "read";
  } else {
    assert(AccessKind == PAQAccessKind::Write && "Unexpected PAQ access kind!");
    Stream << "write";
  }
  return Stream;
}

void PAQAccessMask::print(llvm::raw_ostream &Stream,
                          std::optional<PAQAccessKind> RestrAccessKind) const {
  for (PAQAccessKind AccessKind : {PAQAccessKind::Write, PAQAccessKind::Read}) {
    if (RestrAccessKind && AccessKind != RestrAccessKind)
      continue;
    Stream << AccessKind << "(";
    bool IsFirst = true;
    for (PAQShaderStage ShaderStage : PAQShaderStages) {
      if (get(ShaderStage, AccessKind)) {
        if (!IsFirst)
          Stream << ", ";

        Stream << ShaderStage;
        IsFirst = false;
      }
    }
    Stream << ")";

    if (AccessKind == PAQAccessKind::Write && !RestrAccessKind)
      Stream << " : ";
  }
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Stream,
                                    PAQLifetimeClass LifetimeClass) {
  StringRef String = [LifetimeClass]() {
    switch (LifetimeClass) {
    case PAQLifetimeClass::Caller_To_Caller:
      return "caller to caller";
    case PAQLifetimeClass::Caller_To_ClosestHit:
      return "caller to closesthit";
    case PAQLifetimeClass::Caller_To_ClosestHitAndMiss:
      return "caller to closesthit+miss";
    case PAQLifetimeClass::AnyHit_To_Caller:
      return "anyhit to caller";
    case PAQLifetimeClass::AnyHit_To_ClosestHitAndMiss:
      return "anyhit to closesthit+miss";
    case PAQLifetimeClass::AnyHit_To_ClosestHit:
      return "anyhit to closesthit";
    case PAQLifetimeClass::Caller_To_AnyHit:
      return "caller to anyhit";
    case PAQLifetimeClass::AnyHit_To_AnyHit:
      return "anyhit to anyhit";
    case PAQLifetimeClass::ClosestHitAndMiss_To_Caller:
      return "closesthit+miss to caller";
    case PAQLifetimeClass::ClosestHit_To_Caller:
      return "closesthit to caller";
    case PAQLifetimeClass::Miss_To_Caller:
      return "miss to caller";
    case PAQLifetimeClass::Count:
      break;
    }
    llvm_unreachable("Unknown lifetime class");
  }();
  Stream << String;
  return Stream;
}

static std::string
determineSerializationInfoPrefix(const PAQPayloadConfig &PAQConfig) {
  std::string Result;
  raw_string_ostream Str{Result};
  Str << PAQConfig.PayloadTy->getStructName();
  if (PAQConfig.MaxHitAttributeByteCount != 0) {
    assert(PAQConfig.MaxHitAttributeByteCount % RegisterBytes == 0);
    Str << ".attr_max_" << PAQConfig.MaxHitAttributeByteCount / RegisterBytes
        << "_i32s";
  }
  return Result;
}

// OptLayoutKind is set for TraceRay
// OptNumHitAttrI32s is only set if we are generating a specialized layout for a
// particular hit attribute size obtained from the actual hit attribute type
// (not the max hit attribute size)
static std::string
determineLayoutSuffix(std::optional<PAQSerializationLayoutKind> OptLayoutKind,
                      std::optional<uint32_t> OptNumPayloadHitAttrI32s) {
  std::string Result;
  raw_string_ostream Str{Result};
  Str << "layout_";
  if (OptLayoutKind.has_value()) {
    // TraceRay
    Str << static_cast<int>(*OptLayoutKind) << "_" << *OptLayoutKind;
  } else {
    Str << "callshader";
  }
  if (OptNumPayloadHitAttrI32s.has_value()) {
    assert(OptLayoutKind.has_value());
    Str << ".payload_attr_" << OptNumPayloadHitAttrI32s << "_i32s";
  }
  return Result;
}

// Also used to determine the names of serialization structs,
// hence no spaces are used.
llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Stream,
                                    PAQSerializationLayoutKind LayoutKind) {
  StringRef Identifier = [LayoutKind]() {
    switch (LayoutKind) {
    case PAQSerializationLayoutKind::CallerOut:
      return "caller_out";
    case PAQSerializationLayoutKind::AnyHitIn:
      return "anyhit_in";
    case PAQSerializationLayoutKind::AnyHitOutAcceptHit:
      return "anyhit_out_accept";
    case PAQSerializationLayoutKind::ClosestHitIn:
      return "closesthit_in";
    case PAQSerializationLayoutKind::ClosestHitOut:
      return "closesthit_out";
    case PAQSerializationLayoutKind::MissIn:
      return "miss_in";
    case PAQSerializationLayoutKind::MissOut:
      return "miss_out";
    case PAQSerializationLayoutKind::Count:
      break;
    }
    llvm_unreachable("Unknown layout kind");
  }();
  Stream << Identifier;
  return Stream;
}

std::optional<PAQSerializationLayoutKind>
llvm::tryDetermineLayoutKind(PAQShaderStage ShaderStage,
                             PAQAccessKind AccessKind) {
  assert((AccessKind == PAQAccessKind::Read ||
          AccessKind == PAQAccessKind::Write) &&
         "Invalid access kind!");
  switch (ShaderStage) {
  case PAQShaderStage::Caller: {
    if (AccessKind == PAQAccessKind::Write)
      return PAQSerializationLayoutKind::CallerOut;
    // no unique layout kind in this case
    return {};
  }
  case PAQShaderStage::AnyHit: {
    if (AccessKind == PAQAccessKind::Read)
      return PAQSerializationLayoutKind::AnyHitIn;
    // There are multiple outgoing layouts for anyhit
    return {};
  }
  case PAQShaderStage::ClosestHit: {
    if (AccessKind == PAQAccessKind::Read)
      return PAQSerializationLayoutKind::ClosestHitIn;
    return PAQSerializationLayoutKind::ClosestHitOut;
  }
  case PAQShaderStage::Miss: {
    if (AccessKind == PAQAccessKind::Read)
      return PAQSerializationLayoutKind::MissIn;
    return PAQSerializationLayoutKind::MissOut;
  }
  case PAQShaderStage::Count:
    break;
  }
  llvm_unreachable("invalid shader stage");
}

static void printPAQNodeImpl(llvm::raw_ostream &Stream, const PAQNode &Node,
                             int Depth) {
  Stream.indent(2 * (Depth + 1));

  // Print mask
  Stream << "Access: ";
  if (Node.AccessMask) {
    // Print partially manually to align access masks
    for (PAQAccessKind AccessKind :
         {PAQAccessKind::Write, PAQAccessKind::Read}) {
      uint64_t Begin = Stream.tell();
      Node.AccessMask->print(Stream, AccessKind);
      uint64_t CharsWritten = Stream.tell() - Begin;
      if (AccessKind == PAQAccessKind::Write) {
        Stream << " : ";
      } else {
        Stream << ", ";
      }
      const uint64_t MinWidth = 30;
      if (CharsWritten < MinWidth)
        Stream.indent(MinWidth - CharsWritten);
    }
  } else {
    Stream << "<no access mask, using access flags of nested payload struct>, ";
    // Align with case above
    Stream.indent(2);
  }
  Stream << "Lifetime: ";

  // Print lifetime class
  if (Node.LifetimeClass) {
    Stream << Node.LifetimeClass.value();
  } else {
    Stream << "<no lifetime class>";
  }

  if (Node.Ty->isStructTy()) {
    Stream << ", Type: " << Node.Ty->getStructName();
  }
  Stream << "\n";

  for (unsigned int I = 0; I < Node.Children.size(); ++I)
    printPAQNodeImpl(Stream, Node.Children[I], Depth + 1);
}

void PAQNode::print(llvm::raw_ostream &Stream) const {
  printPAQNodeImpl(Stream, *this, 0);
}

void PAQNodeStorageInfo::print(raw_ostream &O) const {
  if (IndexIntervals.size() > 1)
    O << "{ ";
  bool IsFirst = true;
  for (const PAQIndexInterval &Interval : IndexIntervals) {
    if (!IsFirst)
      O << ", ";
    IsFirst = false;
    O << "[" << Interval.Begin << ", " << Interval.End << ")";
  }
  if (IndexIntervals.size() > 1)
    O << " }";
}

void PAQSerializationLayout::print(raw_ostream &O, bool SingleLine) const {
  if (!SerializationTy) {
    O << "<empty serialization layout>\n";
    return;
  }

  // Sort by interval for output
  SmallVector<std::pair<const PAQNode *, PAQNodeStorageInfo>, 32>
      SortedNodeStorageInfosVector(NodeStorageInfos.begin(),
                                   NodeStorageInfos.end());
  llvm::sort(SortedNodeStorageInfosVector,
             [](const auto &LHS, const auto &RHS) {
               return LHS.second.IndexIntervals < RHS.second.IndexIntervals;
             });

  if (SingleLine) {
    O << *SerializationTy << " ; { ";
    bool First = true;
    for (const auto &NodeWithInfo : SortedNodeStorageInfosVector) {
      if (!First)
        O << ", ";
      First = false;
      O << *NodeWithInfo.first->Ty << ": " << NodeWithInfo.second;
    }
    O << " }\n";
    return;
  }

  assert(PayloadRootNode);
  auto *Indent = "  ";

  O << "Serialization layout for type " << PayloadRootNode->Ty->getStructName()
    << "\n";

  // Print type with body
  O << Indent << "Serialization struct type: " << *SerializationTy << "\n";

  // special nodes: mem ptr and hit attributes
  O << Indent << "Payload memory pointer: ";
  if (PayloadMemPointerNode) {
    auto It = NodeStorageInfos.find(PayloadMemPointerNode);
    assert(It != NodeStorageInfos.end());
    O << "at " << It->second;
  } else {
    O << "no";
  }
  O << "\n";

  O << Indent << "Hit attributes: ";
  if (HitAttributeStorageNode) {
    auto It = NodeStorageInfos.find(HitAttributeStorageNode);
    assert(It != NodeStorageInfos.end());
    O << "at " << It->second;
  } else {
    O << "no";
  }
  O << "\n";

  O << Indent << "Node storage intervals:\n";
  for (const auto &NodeWithInfo : SortedNodeStorageInfosVector) {
    O << Indent << Indent << *NodeWithInfo.first->Ty << " at "
      << NodeWithInfo.second << "\n";
  }
}

void PAQSerializationLayout::dump() const { print(dbgs()); }

// In DXIL Metadata, read/write qualifiers are encoded in a bitmask with a
// single bit per combination of read or write and shader stage.
// Read access is in first bit, write in second, third and fourth bit are
// unused
//
// Stage      Bits
// ----------------
// Caller     0-3
// Closesthit 4-7
// Miss       8-11
// Anyhit     12-15
static PAQAccessMask importPAQAccessMaskFromDXILBitMask(uint32_t DXILBitMask) {
  auto GetAccessOffset = [](PAQAccessKind AccessKind) {
    return AccessKind == PAQAccessKind::Read ? 0 : 1;
  };
  auto GetStageOffset = [](PAQShaderStage ShaderStage) {
    switch (ShaderStage) {
    case PAQShaderStage::Caller:
      return 0;
    case PAQShaderStage::ClosestHit:
      return 4;
    case PAQShaderStage::Miss:
      return 8;
    case PAQShaderStage::AnyHit:
      return 12;
    case llvm::PAQShaderStage::Count:
      break;
    }
    llvm_unreachable("Unknown stage");
  };

  PAQAccessMask Result;
  for (PAQShaderStage Stage : PAQShaderStages) {
    for (PAQAccessKind AccessKind :
         {PAQAccessKind::Read, PAQAccessKind::Write}) {

      uint64_t Offset = GetAccessOffset(AccessKind) + GetStageOffset(Stage);
      Result.set(Stage, AccessKind, DXILBitMask & (1u << Offset));
    }
  }
  return Result;
}

// Constexpr so we can test with static_asserts
static constexpr PAQLifetimeClass
lifetimeClassFromAccessMask(PAQAccessMask AccessMask) {
  using Stage = PAQShaderStage;
  if (AccessMask.get(Stage::Caller, PAQAccessKind::Write)) {
    if (AccessMask.get(Stage::Caller, PAQAccessKind::Read))
      return PAQLifetimeClass::Caller_To_Caller;
    if (AccessMask.get(Stage::Miss, PAQAccessKind::Read))
      return PAQLifetimeClass::Caller_To_ClosestHitAndMiss;
    if (AccessMask.get(Stage::ClosestHit, PAQAccessKind::Read))
      return PAQLifetimeClass::Caller_To_ClosestHit;
    assert(AccessMask.get(Stage::AnyHit, PAQAccessKind::Read) &&
           "Unexpected access mask!");
    return PAQLifetimeClass::Caller_To_AnyHit;
  }
  // write(caller) is unset
  if (AccessMask.get(Stage::AnyHit, PAQAccessKind::Write)) {
    if (AccessMask.get(Stage::Caller, PAQAccessKind::Read))
      return PAQLifetimeClass::AnyHit_To_Caller;
    if (AccessMask.get(Stage::Miss, PAQAccessKind::Read))
      return PAQLifetimeClass::AnyHit_To_ClosestHitAndMiss;
    if (AccessMask.get(Stage::ClosestHit, PAQAccessKind::Read))
      return PAQLifetimeClass::AnyHit_To_ClosestHit;
    assert(AccessMask.get(Stage::AnyHit, PAQAccessKind::Read) &&
           "Unexpected access mask!");
    return PAQLifetimeClass::AnyHit_To_AnyHit;
  }
  // write(caller, anyhit) are unset
  assert(AccessMask.get(Stage::Caller, PAQAccessKind::Read) &&
         "Unexpected PAQ access mask!");
  if (AccessMask.get(Stage::ClosestHit, PAQAccessKind::Write)) {
    if (AccessMask.get(Stage::Miss, PAQAccessKind::Write))
      return PAQLifetimeClass::ClosestHitAndMiss_To_Caller;
    return PAQLifetimeClass::ClosestHit_To_Caller;
  }
  assert(AccessMask.get(Stage::Miss, PAQAccessKind::Write) &&
         "Unexpected PAQ access mask!");
  return PAQLifetimeClass::Miss_To_Caller;
}

// Helper namespace containing testing code for lifetimeClassFromAccessMask
namespace lifetimeTest {
static constexpr PAQAccessMask makeMask(PAQShaderStage WriteStage,
                                        PAQShaderStage ReadStage) {
  PAQAccessMask Result;
  Result.set(WriteStage, PAQAccessKind::Write);
  Result.set(ReadStage, PAQAccessKind::Read);
  return Result;
}
using Stage = PAQShaderStage;
using Lifetime = PAQLifetimeClass;

static_assert(lifetimeClassFromAccessMask(makeMask(Stage::Caller,
                                                   Stage::Caller)) ==
                  Lifetime::Caller_To_Caller,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::Caller,
                                                   Stage::ClosestHit)) ==
                  Lifetime::Caller_To_ClosestHit,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::Caller,
                                                   Stage::Miss)) ==
                  Lifetime::Caller_To_ClosestHitAndMiss,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::Caller,
                                                   Stage::AnyHit)) ==
                  Lifetime::Caller_To_AnyHit,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::AnyHit,
                                                   Stage::Caller)) ==
                  Lifetime::AnyHit_To_Caller,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::AnyHit,
                                                   Stage::ClosestHit)) ==
                  Lifetime::AnyHit_To_ClosestHit,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::AnyHit,
                                                   Stage::Miss)) ==
                  Lifetime::AnyHit_To_ClosestHitAndMiss,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::AnyHit,
                                                   Stage::AnyHit)) ==
                  Lifetime::AnyHit_To_AnyHit,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::ClosestHit,
                                                   Stage::Caller)) ==
                  Lifetime::ClosestHit_To_Caller,
              "Invalid lifetime class!");
static_assert(lifetimeClassFromAccessMask(makeMask(Stage::Miss,
                                                   Stage::Caller)) ==
                  Lifetime::Miss_To_Caller,
              "Invalid lifetime class!");
static_assert(
    lifetimeClassFromAccessMask(makeMask(Stage::ClosestHit, Stage::Caller)
                                    .set(Stage::Miss, PAQAccessKind::Write)) ==
        Lifetime::ClosestHitAndMiss_To_Caller,
    "Invalid lifetime class!");
} // namespace lifetimeTest

std::optional<int64_t> tryExtractSExtIntegerFromMDOp(const MDOperand &Op) {
  auto *Val = mdconst::dyn_extract<ConstantInt>(Op);
  if (!Val)
    return {};
  return Val->getSExtValue();
}

// Imports the PAQ qualifiers for the direct, non-nested fields in
// PayloadType. Nested structs are not resolved, that is, the child nodes in
// the returned object corresponding to the fields in PayloadType have no
// children yet. If TypeAnnotationMDTuple is nullptr, all fields have
// write(all) + read(all) access masks.
static std::unique_ptr<PAQNode>
createPayloadRootNode(Type &PayloadType, MDTuple *TypeAnnotationMDTuple) {
  StructType *PayloadStructType = dyn_cast<StructType>(&PayloadType);
  if (!PayloadStructType)
    report_fatal_error("Unexpected non-struct annotated payload type");

  uint32_t NumElements = PayloadStructType->getNumElements();
  if (TypeAnnotationMDTuple &&
      NumElements != TypeAnnotationMDTuple->getNumOperands())
    report_fatal_error("Incorrect number of metadata entries");

  std::unique_ptr<PAQNode> RootNode =
      std::make_unique<PAQNode>(PAQNode{&PayloadType});
  RootNode->Children.reserve(NumElements);

  for (uint32_t I = 0; I < NumElements; ++I) {
    PAQNode ChildNode = {PayloadStructType->getElementType(I)};
    if (TypeAnnotationMDTuple) {
      // TypeAnnotationMDTuple should contain a nested tuple for every
      // element, consisting of a tag i32, and the bitmask i32.
      const MDOperand &FieldOperand = TypeAnnotationMDTuple->getOperand(I);
      MDTuple *FieldMDTuple = dyn_cast<MDTuple>(FieldOperand);
      if (!FieldMDTuple || FieldMDTuple->getNumOperands() != 2)
        report_fatal_error("Unexpected metadata format");

      std::optional<int64_t> OptTag =
          tryExtractSExtIntegerFromMDOp(FieldMDTuple->getOperand(0));
      std::optional<int64_t> BitMask =
          tryExtractSExtIntegerFromMDOp(FieldMDTuple->getOperand(1));
      constexpr int64_t KDxilPayloadFieldAnnotationAccessTag = 0;
      if (OptTag != KDxilPayloadFieldAnnotationAccessTag ||
          !BitMask.has_value())
        report_fatal_error("Unexpected metadata format");

      // Only import bitmask if the value is non-zero.
      // Otherwise, keep a non-set Optional as opposed to
      // an Optional containing an empty mask.
      // There are two cases in which BitMask is zero:
      //  - The field is qualified as write() : read().
      //  - The field is of nested payload type and thus not qualified,
      //    and qualifiers need to be deduced from the nested payload type.
      // We cannot differentiate between the two cases here.
      // In the second case, a non-set Optional is the right thing,
      // because indeed there is no mask.
      // In the first case, an empty mask would be cleaner.
      // But because we don't know the case, we keep the non-set Optional
      // in both cases, and differentiate later on to assign the empty mask
      // for the first case.
      if (BitMask.value() != 0) {
        ChildNode.AccessMask =
            importPAQAccessMaskFromDXILBitMask(BitMask.value());
      }
    } else {
      // No metadata available, assume all read/all write
      ChildNode.AccessMask.emplace();
      for (PAQShaderStage Stage : PAQShaderStages) {
        ChildNode.AccessMask->set(Stage, PAQAccessKind::Write);
        ChildNode.AccessMask->set(Stage, PAQAccessKind::Read);
      }
    }

    RootNode->Children.push_back(std::move(ChildNode));
  }

  return RootNode;
}

// Recursive implementation for createNestedStructHierarchy.
//
// Creates child nodes, and sets the lifetime class. The access mask
// of Node is set by the caller.
// For leaves, the lifetime class is set from the access mask (if set).
// For inner nodes, the lifetime class is propagated from children if uniform.
static void createNestedStructHierarchyRecursively(
    Type *Ty, PAQNode &Node,
    const MapVector<Type *, std::unique_ptr<PAQNode>> *ModulePayloadRootNodes) {
  assert(Node.Children.empty() && "PAQ hierarchy already created!");

  // If Node.AccessMask is unset, there are two possible cases:
  //  - Node is a nested payload field. In this case, the field was *not*
  //    access-qualified in HLSL, and qualifiers of Node's children are derived
  //    from the qualifiers for Node's payload type.
  //  - Node is a non-payload field with trivial qualifiers: write() : read()
  // These two cases cannot be distinguished earlier on (both are represented by
  // an all-zero bitmask), so both arrive here with a non-set Node.AccessMask.
  //
  // We will first check whether Node is of nested payload type. If so,
  // children's access masks are obtained from the nested payload type's
  // qualifiers (looked up via ModulePAQRootNodes).
  // Otherwise, we assign the empty access mask.

  // Note that depending on processing order, we might not yet have completed
  // computing the hierarchy for the nested payload. That is not a problem,
  // because here we only depend on the root node of the nested payload, which
  // is already available.
  //
  // ModulePAQRootNodes may be nullptr if we are currently creating a
  // hierarchy on-demand and not during the initial import of DXIL metadata,
  // because at this later stage the map of PAQ root nodes no longer exists.
  // However, in this case the whole struct is write(all) + read(all) anyways,
  // and nested payload structs can be ignored.
  const PAQNode *PayloadTypeRootNode = nullptr;
  StructType *StructTy = dyn_cast<StructType>(Ty);
  if (!Node.AccessMask) {
    bool IsNestedPayload = false;
    if (StructTy) {
      assert(ModulePayloadRootNodes != nullptr &&
             "Missing module payload root nodes!");
      auto It = ModulePayloadRootNodes->find(StructTy);
      if (It != ModulePayloadRootNodes->end()) {
        IsNestedPayload = true;
        if (It->second.get() != &Node) {
          PayloadTypeRootNode = It->second.get();
          assert(PayloadTypeRootNode->Children.size() ==
                     StructTy->getNumElements() &&
                 "Inconsistent number of elements in payload PAQ node!");
        }
      }
    }
    if (!IsNestedPayload) {
      // Node must be a non-read/non-write-qualified field (write() : read()).
      // Assign empty access mask.
      Node.AccessMask = PAQAccessMask{};
    }
  }

  if (!StructTy || StructTy->getNumElements() == 0) {
    // Leaf in the tree. Compute lifetime class and return.
    if (Node.AccessMask && !Node.AccessMask->empty()) {
      Node.LifetimeClass = lifetimeClassFromAccessMask(Node.AccessMask.value());
    }
    return;
  }

  Node.Children.reserve(StructTy->getNumElements());
  bool LifetimeClassesAreUniform = true;
  // Construct child nodes, and propagate their lifetime class if uniform
  for (uint32_t I = 0; I < StructTy->getNumElements(); ++I) {
    Type *ChildTy = StructTy->getElementType(I);
    PAQNode ChildNode{ChildTy};
    if (Node.AccessMask) {
      // Use access mask from parent
      ChildNode.AccessMask = Node.AccessMask;
    } else if (PayloadTypeRootNode) {
      // Use access mask from payload type definition
      // May be unset if ChildTy is again a payload struct type
      ChildNode.AccessMask = PayloadTypeRootNode->Children[I].AccessMask;
    }

    createNestedStructHierarchyRecursively(ChildTy, ChildNode,
                                           ModulePayloadRootNodes);
    Node.Children.push_back(std::move(ChildNode));

    if (Node.Children.back().LifetimeClass !=
        Node.Children.front().LifetimeClass)
      LifetimeClassesAreUniform = false;
  }
  if (LifetimeClassesAreUniform)
    Node.LifetimeClass = Node.Children[0].LifetimeClass;
}

[[maybe_unused]] static void dumpPAQTree(StructType *PayloadType,
                                         const PAQNode &Node) {
  // print for testing
  llvm::dbgs() << "PAQ qualifiers for payload struct " << PayloadType->getName()
               << ":\n";
  for (const auto &Child : Node.Children)
    llvm::dbgs() << Child;

  llvm::dbgs() << "End of PAQ qualifiers\n";
}

// Computes the full PAQ hierarchy tree for a payload struct.
// A payload may contain non-access-qualified fields
// of payload type, in which case qualifiers of nested fields need to
// be determined from the nested payload type.
// Hence, a map of all root nodes of payload structs in the module is passed.
// These are not yet hierarchically expanding (because that is what this
// function does), which is fine because only the root nodes are accessed.
// ModulePayloadRootNodes may be nullptr, in which case no unqualified fields
// may exist in Node.
// Note that setting an access mask for a node applies the same mask to its
// whole subtree.
static void createNestedStructHierarchy(
    Type *PayloadType, PAQNode &Node,
    const MapVector<Type *, std::unique_ptr<PAQNode>> *ModulePayloadRootNodes) {
  StructType *StructTy = cast<StructType>(PayloadType);
  for (uint32_t I = 0; I < StructTy->getNumElements(); ++I) {
    PAQNode &ChildNode = Node.Children[I];
    createNestedStructHierarchyRecursively(StructTy->getElementType(I),
                                           ChildNode, ModulePayloadRootNodes);
  }

  LLVM_DEBUG(dumpPAQTree(StructTy, Node));
}

static std::unique_ptr<PAQNode>
createTrivialHierarchicalPayloadRootNode(Type &PayloadType) {
  std::unique_ptr<PAQNode> RootNode =
      createPayloadRootNode(PayloadType, nullptr);
  assert(RootNode && "Failed to create PAQ tree for payload type");
  createNestedStructHierarchy(&PayloadType, *RootNode, nullptr);
  return RootNode;
}

// Import PAQ access qualifiers encoded in DXIL metadata. Format example:
// clang-format off
//
// ```hlsl
//   struct [raypayload] SimplePayload
//   {
//       float v1 : write(caller)     : read(caller);
//       float v2 : write(caller)     : read(anyhit);
//       float v3 : write(caller)     : read(anyhit);
//   };
//
//   struct [raypayload] OtherSimplePayload
//   {
//       float v1 : write(caller)     : read(caller);
//       float v2 : write(closesthit) : read(caller);
//   };
// ```
//
// is compiled by DXC to
//
// ```dxil
//   !dx.dxrPayloadAnnotations = !{!14}
//
//   ; The 0 (kDxilPayloadAnnotationStructTag) marks begin of a PAQ list.
//   ; The list consists of pairs (Type undef, !Node), where !Node contains
//   ; the PAQ qualifiers for Type.
//   !14 = !{i32 0, %struct.SimplePayload undef, !15, %struct.OtherSimplePayload undef, !18}
//
//   ; List for SimplePayload. One node for every field in SimplePayload
//   !15 = !{!16, !17, !17}
//   ; The 0 (kDxilPayloadFieldAnnotationAccessTag) marks a field annotation, the second int is a bitmask
//   ; imported by importPAQAccessMaskFromDXILBitMask
//   !16 = !{i32 0, i32 3}
//   !17 = !{i32 0, i32 4098}
//
//   ; List for OtherSimplePayload.
//   !18 = !{!16, !19}
//   !19 = !{i32 0, i32 33}
// ```
// clang-format on
//
// This function only imports qualifiers on direct members from DXIL metadata.
// Recursive traversal of nested structs is done later, using the annotations on
// the top-level payload structs collected in this first phase.
static MapVector<Type *, std::unique_ptr<PAQNode>>
importModulePAQRootNodes(const Module &M) {
  LLVM_DEBUG(dbgs() << "Importing DXIL PAQ metadata\n");
  auto *MDName = "dx.dxrPayloadAnnotations";
  auto *MD = M.getNamedMetadata(MDName);
  if (!MD) {
    LLVM_DEBUG(dbgs() << "PAQ: metadata " << MDName
                      << " not found, skipping PAQ import\n");
    return {};
  }

  // Traverse the operands, and check that there is a unique node that is a
  // list starting with the value KDxilPayloadAnnotationStructTag.
  MDNode *AnnotationMDTup = nullptr;
  for (MDNode *Annot : MD->operands()) {
    LLVM_DEBUG(dbgs() << "PAQ annotation: " << *Annot << "\n");
    MDTuple *MDTup = dyn_cast<MDTuple>(Annot);
    if (!MDTup || MDTup->getNumOperands() == 0)
      continue;
    std::optional<int64_t> OptTag =
        tryExtractSExtIntegerFromMDOp(MDTup->getOperand(0));
    constexpr int64_t KDxilPayloadAnnotationStructTag = 0;
    if (OptTag != KDxilPayloadAnnotationStructTag)
      continue;

    // Success: Found correct metadata node
    if (AnnotationMDTup)
      report_fatal_error("Duplicate payload struct annotation metadata nodes!");
    else
      AnnotationMDTup = MDTup;
  }
  if (!AnnotationMDTup) {
    LLVM_DEBUG(dbgs() << "PAQ: failed to find struct annotation node, "
                         "skipping PAQ import\n");
    return {};
  }

  // Check length: One tag node, plus type/node pairs, so must be odd.
  if (AnnotationMDTup->getNumOperands() % 2 != 1)
    report_fatal_error("Unexpected even tuple length!");

  MapVector<Type *, std::unique_ptr<PAQNode>> PayloadRootNodes;
  // Traverse type/node pairs
  for (uint32_t I = 1; I + 1 < AnnotationMDTup->getNumOperands(); I += 2) {
    const MDOperand &TypeOperand = AnnotationMDTup->getOperand(I);
    auto *TypeConstMD = dyn_cast<ConstantAsMetadata>(TypeOperand);
    const MDOperand &NodeOperand = AnnotationMDTup->getOperand(I + 1);
    auto *TypeAnnotationMDTuple = dyn_cast<MDTuple>(NodeOperand);

    if (!TypeConstMD || !TypeAnnotationMDTuple)
      report_fatal_error("Unexpected metadata format.");

    Type *PayloadType = TypeConstMD->getType();
    std::unique_ptr<PAQNode> RootNode =
        createPayloadRootNode(*PayloadType, TypeAnnotationMDTuple);
    bool Inserted =
        PayloadRootNodes.insert({PayloadType, std::move(RootNode)}).second;
    (void)Inserted;
    assert(Inserted && "Duplicate PayloadType in result map!");
  }

  return PayloadRootNodes;
}

// Computes PAQ trees for all payload types for which DXIL payload annotation
// metadata is present. For payload types without annotations, trivial
// PAQ trees are created later on demand.
static MapVector<Type *, std::unique_ptr<PAQNode>>
importModulePayloadPAQNodes(const Module &M) {
  // Import from metadata. This needs to happen for all structs
  // before we recursively traverse field members, because
  // payload fields can be of payload struct type, in which case
  // the qualifiers are obtained from its type.
  MapVector<Type *, std::unique_ptr<PAQNode>> PayloadRootNodes =
      importModulePAQRootNodes(M);

  // Recursively create the nested struct hierarchy
  for (auto &TypeWithInfo : PayloadRootNodes) {
    createNestedStructHierarchy(TypeWithInfo.first, *TypeWithInfo.second,
                                &PayloadRootNodes);
  }

  return PayloadRootNodes;
}

void PAQNode::collectLeafNodes(SmallVectorImpl<const PAQNode *> &Result) const {
  if (Ty->isStructTy()) {
    // If Node.LifetimeClass is set, we could keep the struct together
    // instead of dissolving it into its elements, but dissolving
    // has the advantage to reduce potential padding.
    // Node.Children may be empty for empty structs,
    // leading to intentionally non-represented subtrees.
    for (const PAQNode &ChildNode : Children)
      ChildNode.collectLeafNodes(Result);
  } else {
    // Fields with write() : read() have no lifetime class
    // and are not collected for serialization
    if (LifetimeClass)
      Result.push_back(this);
  }
}

// Checks properties of PAQSerializationLayout:
//  - payload mem ptr properties comes first if present
//  - size of storage intervals
//  - storage intervals are disjoint
//    Note that this property should always hold for layouts that are
//    automatically computed by LayoutComputer. However, in principle, we could
//    have layouts with overlapping storage for fields that cannot be
//    simultaneously live. For example, a "write(closesthit)" field may share
//    storage with a "write(miss)" field in a CallerIn layout.
static MapVector<const PAQNode *, PAQIndexIntervals>
checkSerializationLayout(const PAQSerializationLayout &Layout,
                         const DataLayout &DL) {
  StructType *SerializationTy = Layout.SerializationTy;
  if (!SerializationTy)
    return {};

  // Check mem pointer storage
  if (Layout.PayloadMemPointerNode) {
    auto It = Layout.NodeStorageInfos.find(Layout.PayloadMemPointerNode);
    if (It == Layout.NodeStorageInfos.end())
      report_fatal_error("Missing payload memory pointer!");
    PAQIndexIntervals MemPointerIntervals = It->second.IndexIntervals;
    if (MemPointerIntervals.size() != 1 || MemPointerIntervals[0].size() != 1)
      report_fatal_error("Payload memory pointer must be a single I32!");
    if (MemPointerIntervals[0].Begin != FirstPayloadMemoryPointerRegister)
      report_fatal_error("Payload memory pointer at incorrect offset!");
  }

  BitVector UsedIndices(Layout.NumStorageI32s, false);
  MapVector<const PAQNode *, PAQIndexIntervals> Result;

  for (const auto &NodeWithInfo : Layout.NodeStorageInfos) {
    const PAQNode *Node = NodeWithInfo.first;
    if (!Node)
      report_fatal_error("Layout contains nullptr node!");
    PAQIndexIntervals Intervals = NodeWithInfo.second.IndexIntervals;
    unsigned NumI32s = 0;
    // Count used I32s, and check for overlaps
    for (const auto &Interval : Intervals) {
      NumI32s += Interval.size();
      for (unsigned I = Interval.Begin; I != Interval.End; ++I) {
        if (UsedIndices[I])
          report_fatal_error("Storage index is used multiple times!");
        UsedIndices.set(I);
      }
    }
    // Check size
    unsigned StoreSize = DL.getTypeStoreSize(Node->Ty).getFixedValue();
    unsigned RequiredNumI32s = divideCeil(StoreSize, RegisterBytes);
    if (NumI32s != RequiredNumI32s)
      report_fatal_error("Incorrect serialization size!");
    Result.insert({Node, Intervals});
  }
  return Result;
}

// Checks all individual serialization layouts using checkSerializationLayout,
// and then checks consistency across different layouts, in particular that
// offsets of the same PAQNodes are the same in all layouts.
// In some cases (e.g. for hit attribute storage), we use different PAQNodes to
// refer to the same data in different layouts. To check these as well, keys in
// EquivalentNodes are treated as if they were their corresponding values.
// Note: Currently, we only uses leaves of the PAQ tree in serialization
//       structs. If at some point we also use inner nodes in serialization
//       structs, we should also check consistency between a node and its
//       ancestors (i.e. parent structs).
[[maybe_unused]] static void checkTraceRaySerializationInfoImpl(
    ArrayRef<const PAQSerializationLayout *> Layouts,
    const SmallDenseMap<const PAQNode *, const PAQNode *> &EquivalentNodes,
    const DataLayout &DL) {
  MapVector<const PAQNode *, PAQIndexIntervals> MergedNodeIntervals;
  for (const PAQSerializationLayout *Layout : Layouts) {
    StructType *SerializationTy = Layout->SerializationTy;
    if (!SerializationTy) {
      if (!Layout->NodeStorageInfos.empty())
        report_fatal_error(
            "Empty serialization struct but non-empty contained fields!");
      continue;
    }
    MapVector<const PAQNode *, PAQIndexIntervals> NodeIntervals =
        checkSerializationLayout(*Layout, DL);

    for (const auto &NodeWithIntervals : NodeIntervals) {
      const PAQNode *Node = NodeWithIntervals.first;
      const PAQIndexIntervals &Intervals = NodeWithIntervals.second;
      auto It = EquivalentNodes.find(Node);
      bool IsEquivalent = false;
      if (It != EquivalentNodes.end()) {
        // Replace node by its identified node in the global comparison
        Node = It->second;
        IsEquivalent = true;
      }
      // Try to insert. If already present, compare offsets
      auto InsertionResult = MergedNodeIntervals.insert({Node, Intervals});
      const PAQIndexIntervals &ExistingIntervals =
          InsertionResult.first->second;
      if (!IsEquivalent && Intervals != ExistingIntervals) {
        report_fatal_error("Inconsistent serialization offset!");
      }

      // We compare different nodes that are equivalent, that is,
      // we need to support storing with one node, and loading with the other.
      // In this case we explicitly support different sizes (e.g. for
      // specialized hit attributes), so we cannot just compare the intervals
      // for equality. Instead, check that one range is a prefix of the other.

      // Determine which one should be the small prefix
      const PAQIndexIntervals *PrefixRange = &ExistingIntervals;
      const PAQIndexIntervals *ContainingRange = &Intervals;
      if (PrefixRange->empty() || ContainingRange->empty())
        continue;
      if (ContainingRange->size() < PrefixRange->size() ||
          (ContainingRange->size() == PrefixRange->size() &&
           ContainingRange->back().End < PrefixRange->back().End)) {
        std::swap(PrefixRange, ContainingRange);
      }

      // Now check that PrefixRange is a prefix of ContainingRange
      for (unsigned I = 0; I < PrefixRange->size(); ++I) {
        if (I + 1 < PrefixRange->size()) {
          // All but the last intervals must be the same
          if ((*PrefixRange)[I] != (*ContainingRange)[I])
            report_fatal_error("Inconsistent serialization offset!");
        } else {
          // The last interval must be a prefix, i.e. same Begin
          // and smaller or equal End
          if (((*PrefixRange)[I].Begin != (*ContainingRange)[I].Begin) ||
              ((*PrefixRange)[I].End > (*ContainingRange)[I].End))
            report_fatal_error("Inconsistent serialization offset!");
        }
      }
    }
  }
}

// Checks the contained serialization layouts for consistency,
// ensuring that writing in one layout and reading in another
// yields correct data (for the intersection of fields).
// HitGroupLayouts in TraceRaySerializationInfo are not checked.
// However, if HitGroupLayout is non-null, its consistency with the
// other layouts will be checked as well.
[[maybe_unused]] static void checkTraceRaySerializationInfo(
    const PAQTraceRaySerializationInfo &TraceRaySerializationInfo,
    const DataLayout &DL,
    const PAQHitGroupLayoutInfo *HitGroupLayout = nullptr) {

  SmallVector<const PAQSerializationLayout *,
              static_cast<std::size_t>(PAQSerializationLayoutKind::Count) + 2>
      Layouts{make_pointer_range(TraceRaySerializationInfo.LayoutsByKind)};

  SmallDenseMap<const PAQNode *, const PAQNode *> EquivalentNodes;
  if (HitGroupLayout) {
    // add serialization layouts of hitgroup
    Layouts.push_back(&HitGroupLayout->AnyHitOutAcceptHitLayout);
    Layouts.push_back(&HitGroupLayout->ClosestHitInLayout);
    if (HitGroupLayout->HitAttributesNode) {
      // identify specialized hit group node with the common one to ensure
      // consistent offsets of them
      EquivalentNodes[HitGroupLayout->HitAttributesNode.get()] =
          TraceRaySerializationInfo.WorstCaseHitAttributesNode.get();
    }
  }
  checkTraceRaySerializationInfoImpl(Layouts, EquivalentNodes, DL);
}

// Relative order is only relevant for pairs with shared lifetime
PAQLifetimeClassPackingOrder llvm::determineLifetimeClassPackingOrder() {
  return {PAQLifetimeClass::Caller_To_Caller,
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
}

// Represents a PAQLifetimeClass permutation/ordering by storing for each
// lifetime class its index in the ordering. This allows to quickly determine
// the relative order of two given lifetime classes in the PackingOrder.
using PAQLifetimeClassOrderingIndices =
    llvm::EnumeratedArray<unsigned, PAQLifetimeClass, PAQLifetimeClass::Last,
                          std::size_t>;

static PAQLifetimeClassOrderingIndices computeLifetimeClassOrderingIndices(
    const PAQLifetimeClassPackingOrder &Ordering) {
  PAQLifetimeClassOrderingIndices Result{};
  assert(Result.size() == Ordering.size() && "Inconsistent array lengths!");
  for (PAQLifetimeClass LifetimeClass : PAQLifetimeClasses)
    Result[LifetimeClass] = -1;
  for (unsigned I = 0; I < Ordering.size(); ++I) {
    PAQLifetimeClass LifetimeClass = Ordering[I];
    assert(Result[LifetimeClass] == static_cast<unsigned>(-1) &&
           "Duplicate ordering entry!");
    Result[LifetimeClass] = I;
  }
  return Result;
}

// Returns whether a lifetime class is functionally live in the given layout
// kind. Even if not live, it might be contained as dummy in the layout to
// guarantee stable offsets of other lifetime classes.
static bool isLiveInLayout(PAQLifetimeClass LifetimeClass,
                           PAQSerializationLayoutKind LayoutKind) {
  // Consistent criteria to query whether a lifetime class of type FromXXX
  // or ToXXX is live in LayoutKind
  const bool FromCaller = true;
  const bool FromAnyHit = (LayoutKind != PAQSerializationLayoutKind::CallerOut);
  const bool FromClosestHit =
      (LayoutKind == PAQSerializationLayoutKind::ClosestHitOut);
  const bool FromMiss = (LayoutKind == PAQSerializationLayoutKind::MissOut);
  const bool ToCaller = true;
  const bool ToAnyHit =
      (LayoutKind <= PAQSerializationLayoutKind::AnyHitOutAcceptHit);
  const bool ToClosestHit =
      (LayoutKind != PAQSerializationLayoutKind::MissIn &&
       LayoutKind <= PAQSerializationLayoutKind::ClosestHitIn);
  const bool ToClosestHitAndMiss =
      (LayoutKind <= std::max(PAQSerializationLayoutKind::MissIn,
                              PAQSerializationLayoutKind::ClosestHitIn));

  switch (LifetimeClass) {
  case PAQLifetimeClass::Caller_To_Caller:
    return FromCaller && ToCaller;
  case PAQLifetimeClass::AnyHit_To_Caller: {
    return FromAnyHit && ToCaller;
  }
  case PAQLifetimeClass::Caller_To_ClosestHit: {
    return FromCaller && ToClosestHit;
  }
  case PAQLifetimeClass::Caller_To_ClosestHitAndMiss: {
    return FromCaller && ToClosestHitAndMiss;
  }
  case PAQLifetimeClass::AnyHit_To_ClosestHitAndMiss: {
    return FromAnyHit && ToClosestHitAndMiss;
  }
  case PAQLifetimeClass::AnyHit_To_ClosestHit: {
    return FromAnyHit && ToClosestHit;
  }
  case PAQLifetimeClass::Caller_To_AnyHit: {
    return FromCaller && ToAnyHit;
  }
  case PAQLifetimeClass::AnyHit_To_AnyHit: {
    return FromAnyHit && ToAnyHit;
  }
  case PAQLifetimeClass::ClosestHitAndMiss_To_Caller: {
    return (FromClosestHit || FromMiss) && ToCaller;
  }
  case PAQLifetimeClass::ClosestHit_To_Caller: {
    return FromClosestHit && ToCaller;
  }
  case PAQLifetimeClass::Miss_To_Caller: {
    return FromMiss && ToCaller;
  }
  case PAQLifetimeClass::Count:
    break;
  }
  llvm_unreachable("invalid lifetime class");
}

// A bit mask indexed by a layout index, that is, either 0 (for CallShader)
// or PAQSerializationLayoutKind.
using LayoutBitmask = uint8_t;
static constexpr uint64_t MaxNumLayoutsInBitmask = 8 * sizeof(LayoutBitmask);
static_assert(static_cast<std::size_t>(PAQSerializationLayoutKind::Count) <=
                  MaxNumLayoutsInBitmask,
              "Increase BitMask width");

// Used from LayoutComputer if the serialization does not fit into registers
static std::unique_ptr<PAQNode> createPayloadMemPointerNode(Module &M) {
  PAQAccessMask AccessMask;
  AccessMask.set(PAQShaderStage::Caller, PAQAccessKind::Write);
  for (PAQShaderStage Stage : PAQShaderStages)
    AccessMask.set(Stage, PAQAccessKind::Read);

  // Use a single I32 to store the pointer
  Type *I32 = Type::getInt32Ty(M.getContext());
  return std::make_unique<PAQNode>(
      PAQNode{I32, {}, AccessMask, lifetimeClassFromAccessMask(AccessMask)});
}

namespace {

// Overview
// ========

// Common implementation to create serialization layouts for the complete
// lifetime of a payload, either for all TraceRay stages, or for a single
// CallShader call. The actual functions creating a PAQTraceRaySerializationInfo
// and a PAQCallShaderSerializationInfo are wrappers around this.
//
// The input (CreateInfo) contains a list of layout struct names to be created,
// implicitly defining the number of layouts to be computed, and a set of PAQ
// nodes together with a lifetime bitmask.
//
// The output is a list of serialization layouts. Each node is contained
// in the layouts specified by its lifetime bitmask.
//
// Layouts are structs containing a single I32 array.
// The storage for a node is given by a set of indices into the array,
// represented as intervals to optimize for the common case of consecutive
// indices.
//
// A node uses the same indices in all layouts it is part of.
// Because the explicit, absolute indices are stored, we do not need any "dummy
// nodes". Instead, there might be unused indices in some layouts to ensure
// stable node indices.
//
// The payload memory pointer is not part of the input. Instead, it is
// automatically added to the layout if necessary, that is, if at least one
// layout becomes too large.
//
// Nodes may specify a fixed set of storage indices, used e.g. for hit
// attributes which need to be placed in fixed registers.
//
// For CallShader, just a single layout is computed.
// For TraceRay, one layout per value of PAQSerializationLayoutKind is created.
// However, this correspondence is handled entirely by outside code,
// LayoutComputer is entirely agnostic of any CallShader or TraceRay specifics.
//
// Algorithm
// =========
//
// The assignment of indices works as follows: First, all nodes with fixed
// indices are assigned. The remaining nodes are lexicographically sorted by
// PAQLifetimeClass (according to PackingOrder), and their index in Nodes.
//
// In that order, we greedily allocate I32s, assigning each node the set of
// minimal free indices, incrementally building a 2-D matrix (UsedI32s) that
// specifies for each pair of layout index and I32 index whether that I32 is
// already in use in that layout.
//
// If the lifetimes of nodes form a laminar family, that is, for any two nodes A
// and B, either the lifetime of A contains the one of B, or vice versa,
// this algorithm computes layouts without any holes.
// In particular, the algorithm is optimal in this case.
//
// In other cases, holes might be unavoidable, and in general there is a
// tradeoff on which layouts should contain holes. For this algorithm, this can
// be controlled by the PackingOrder.
//
// We first try to create layouts without a payload memory pointer.
// Once we exceed the payload register size, we stop, add a payload memory
// pointer node info, and repeat the allocation phase.
//
// TODO: Implement a consecutive packing optimization as postopt.
// If the determined index set is non-consecutive, we could try
// to swap some storage intervals with smaller nodes.
// For example, if register 0 is free (because there is no payload pointer),
// but 1-6 are reserved for hit attributes, we could try to pack a single 32-bit
// node into the first register instead of splitting a 64-bit node into
// registers 0 and 7.
class LayoutComputer {
public:
  // Info about a single node to be included in serialization layouts
  struct NodeInfo {
    const PAQNode *Node;
    // If non-empty, this specifies the exact indices of I32s to use as storage
    // for Node.
    PAQIndexIntervals FixedIndexIntervals;
    // Indexed by layout index (w.r.t. CreateInfo::LayoutInfos).
    LayoutBitmask LivenessBitmask;
  };

  // Info about a single serialization layout struct to be computed
  struct LayoutInfo {
    // The name of the struct type to be created
    std::string Name;
  };

  struct CreateInfo {
    Module &M;
    // Set of all node infos to be possibly included in one of the layouts.
    SmallVector<NodeInfo, 16> NodeInfos = {};
    // We generate one result layout per layout info
    SmallVector<LayoutInfo,
                static_cast<std::size_t>(PAQSerializationLayoutKind::Count)>
        LayoutInfos = {};
    // Storage is allocated greedily, ordered by PAQLifetimeClass as in
    // PackingOrder
    PAQLifetimeClassPackingOrder PackingOrder = {};

    // These have no impact on the generated layouts, except that the
    // corresponding fields in the generated PAQSerializationLayout objects are
    // set to these.
    const PAQNode *PayloadRootNode = nullptr;
    const PAQNode *HitAttributeStorageNode = nullptr;
    // Note that there is no input payload mem pointer node,
    // instead, it is created on the fly if necessary.

    // The maximum allowed number of I32s without using a memory pointer
    // Note that the payload memory pointer is automatically created on
    // demand while constructing the layout, and is part of the result.
    unsigned PayloadRegisterCount = 0;
  };

  struct Result {
    // Computed layouts, in order.
    SmallVector<PAQSerializationLayout,
                static_cast<std::size_t>(PAQSerializationLayoutKind::Count)>
        Layouts;

    // Non-null if a payload memory pointer is required
    std::unique_ptr<PAQNode> PayloadMemPointerNode;

    // Maximum number of I32s to store any of the contained layouts
    unsigned MaxNumI32s = 0;
  };

  static Result create(const CreateInfo &CreateInfo) {
    LayoutComputer Creator{CreateInfo};
    return Creator.run();
  };

private:
  LayoutComputer(const CreateInfo &CInfo) : CInfo{CInfo} {}

  Result run() {
    assert(CInfo.LayoutInfos.size() <= MaxNumLayoutsInBitmask &&
           "Too many layouts");

    prepareSortedNodeInfos();
    computeAllocation();
    // TODO:
    // postoptimizeAllocation();
    const I32Allocation &Allocation = OptAllocation.value();

    return createResult(Allocation);
  }

  // Intermediate representation of a set of layouts.
  // The final Result is later constructed from this.
  // Assigns every node (by index in SortedNodeInfos) storage intervals,
  // and keeps track of used indices in UsageMatrix.
  struct I32Allocation {
    // Indexed with the same indexes as SortedNodeInfos.
    // Stores for each node the set of I32s used for storage, represented as
    // intervals.
    SmallVector<PAQIndexIntervals, 16> NodeIndexIntervals;
    // The j-th bit in the i-th entry stores whether the i-th I32 is used (so
    // far) in the j-th layout
    SmallVector<LayoutBitmask> UsageMatrix;

    unsigned numUsedI32s() const { return UsageMatrix.size(); }

    void ensureSize(unsigned Size) {
      while (UsageMatrix.size() < Size)
        UsageMatrix.push_back({});
    }

    // Tries to allocate the given fixed intervals.
    // Returns true on success.
    bool tryAllocateFixedIntervals(const PAQIndexIntervals &FixedIndexIntervals,
                                   LayoutBitmask LivenessBitmask,
                                   unsigned MaxNumI32s,
                                   unsigned &NumAllocatedI32s) {
      NumAllocatedI32s = 0;
      for (const PAQIndexInterval &Interval : FixedIndexIntervals) {
        assert(Interval.size() != 0 && "Trying to allocate empty interval!");
        if (Interval.End > MaxNumI32s) {
          // We are too large
          return false;
        }
        ensureSize(Interval.End);
        for (unsigned I32Index = Interval.Begin; I32Index < Interval.End;
             ++I32Index) {
          // Check for overlap
          if (UsageMatrix[I32Index] & LivenessBitmask)
            return false;
          // Register usage
          UsageMatrix[I32Index] |= LivenessBitmask;
          ++NumAllocatedI32s;
        }
      }
      return true;
    }
  };

  void prepareSortedNodeInfos() {
    // + 1 for mem pointer
    SortedNodeInfos.reserve(CInfo.NodeInfos.size() + 1);
    for (const auto &NodeInfo : CInfo.NodeInfos) {
      SortedNodeInfos.push_back(&NodeInfo);
    }

    PAQLifetimeClassOrderingIndices OrderingIndices =
        computeLifetimeClassOrderingIndices(CInfo.PackingOrder);

    auto GetSortKey =
        [&](const NodeInfo &Info) -> std::tuple<unsigned, unsigned> {
      // Nodes with fixed assignments come first
      unsigned Order = Info.FixedIndexIntervals.empty() ? 1 : 0;
      const auto &OptLifetimeClass = Info.Node->LifetimeClass;
      unsigned LifetimeClassIndex =
          OptLifetimeClass ? OrderingIndices[OptLifetimeClass.value()] : 0;
      return {Order, LifetimeClassIndex};
    };

    // stable_sort so original order is preserved if possible
    std::stable_sort(SortedNodeInfos.begin(), SortedNodeInfos.end(),
                     [GetSortKey](const NodeInfo *LHS, const NodeInfo *RHS) {
                       return GetSortKey(*LHS) < GetSortKey(*RHS);
                     });
  }

  // Sets OptAllocation.
  void computeAllocation() {
    // Try without payload memory pointer
    tryComputeAllocation(CInfo.PayloadRegisterCount);
    if (!OptAllocation) {
      // Failure, try again with
      addPayloadMemPointer();
      tryComputeAllocation(-1);
      if (!OptAllocation) {
        // This can only happen if with inconsistent predefined index intervals
        report_fatal_error("Failed to compute payload serialization layout");
      }
    }
  }

  // Tries to create a layout allocation with at most MaxNumI32s I32s and
  // store it in OptAllocation. Success status can be queried by checking if
  // OptAllocation is set.
  // If there are inconsistent predefined index intervals, we also fail.
  //
  // To avoid dynamic allocations, we use quite large SmallVectors in
  // LayoutsAllocation. As these cannot be cheaply moved, we have the
  // OptAllocation class member that is optionally initialized by this
  // function, instead of just returning the result.
  void tryComputeAllocation(unsigned MaxNumI32s) {
    // Tentatively construct result object. This will be reset if we fail.
    OptAllocation.emplace();
    auto *Allocation = &OptAllocation.value();
    const auto &DL = CInfo.M.getDataLayout();

    // Speedup: Instead of searching for the first free position from scratch
    //          for every node, we continue at the last node's position
    //          unless the liveness bitmask changes.
    struct LastNodeInfo {
      LayoutBitmask Bitmask;
      unsigned NextFreeIndex;
    };
    std::optional<LastNodeInfo> LastNodeInfo;

    for (const NodeInfo *NodeInfo : SortedNodeInfos) {
      const auto LivenessBitmask = NodeInfo->LivenessBitmask;
      const auto *Node = NodeInfo->Node;

      // Determine size
      unsigned NumBytes = DL.getTypeStoreSize(Node->Ty).getFixedValue();
      unsigned NumI32s = divideCeil(NumBytes, RegisterBytes);
      auto &IndexIntervals = Allocation->NodeIndexIntervals.emplace_back();

      // Handle case that the node has pre-assigned indices first
      if (!NodeInfo->FixedIndexIntervals.empty()) {
        unsigned NumAllocatedI32s = 0;
        if (!Allocation->tryAllocateFixedIntervals(
                NodeInfo->FixedIndexIntervals, LivenessBitmask, MaxNumI32s,
                NumAllocatedI32s)) {
          // Failure. Reset allocation and return.
          OptAllocation.reset();
          return;
        }
        assert(NumAllocatedI32s == NumI32s && "Inconsistent storage size!");
        IndexIntervals = NodeInfo->FixedIndexIntervals;
        continue;
      }

      // Non-fixed indices: allocate the first NumI32s free I32s
      // We may later try to re-distribute to avoid non-consecutive storage

      // Check whether we can resume the search at the last node's position
      unsigned I32Index = 0;
      if (LastNodeInfo) {
        if (LastNodeInfo->Bitmask == LivenessBitmask)
          I32Index = LastNodeInfo->NextFreeIndex;
      }

      // Allocate I32s
      for (unsigned RemainingI32s = NumI32s; RemainingI32s > 0; ++I32Index) {
        if (I32Index >= MaxNumI32s) {
          // We are too large, throw away the allocation
          OptAllocation.reset();
          return;
        }
        Allocation->ensureSize(I32Index + 1);

        if ((Allocation->UsageMatrix[I32Index] & LivenessBitmask) == 0) {
          // I32Index is free to be used for the current node
          // Register index as used
          Allocation->UsageMatrix[I32Index] |= LivenessBitmask;
          // Extend existing interval, or add new one
          if (!IndexIntervals.empty() && IndexIntervals.back().End == I32Index)
            ++IndexIntervals.back().End;
          else
            IndexIntervals.push_back({I32Index, I32Index + 1});
          --RemainingI32s;
        }
      }

      LastNodeInfo = {LivenessBitmask, I32Index};
    }

    assert(Allocation->numUsedI32s() <= MaxNumI32s && "Used too many I32s!");
  }

  void addPayloadMemPointer() {
    assert(PayloadMemPointerNode == nullptr &&
           "Payload mem pointer already initialized!");
    PayloadMemPointerNode = createPayloadMemPointerNode(CInfo.M);
    PAQIndexInterval Interval = {FirstPayloadMemoryPointerRegister,
                                 FirstPayloadMemoryPointerRegister + 1};
    PayloadMemPointerNodeInfo =
        NodeInfo{PayloadMemPointerNode.get(), {Interval}, LayoutBitmask(-1)};
    SortedNodeInfos.insert(SortedNodeInfos.begin(),
                           &PayloadMemPointerNodeInfo.value());
  }

  PAQSerializationLayout
  createSerializationLayout(const I32Allocation &Allocation,
                            unsigned LayoutIndex) const {
    PAQSerializationLayout Layout = {};
    for (unsigned NodeIndex = 0; NodeIndex < SortedNodeInfos.size();
         ++NodeIndex) {
      const NodeInfo *NInfo = SortedNodeInfos[NodeIndex];
      assert(NInfo->Node && "Nullptr node in layout!");
      // Check whether this node is included in the current layout
      if ((NInfo->LivenessBitmask & (1u << LayoutIndex)) == 0)
        continue;

      Layout.NodeStorageInfos[NInfo->Node] =
          PAQNodeStorageInfo{Allocation.NodeIndexIntervals[NodeIndex]};

      for (const auto &Interval : Allocation.NodeIndexIntervals[NodeIndex]) {
        assert(Interval.size() != 0 && "Unexpected empty interval!");
        Layout.NumStorageI32s = std::max(Layout.NumStorageI32s, Interval.End);
      }
    }

    if (Layout.NumStorageI32s) {
      Type *I32 = Type::getInt32Ty(CInfo.M.getContext());
      ArrayType *ArrType = ArrayType::get(I32, Layout.NumStorageI32s);
      Layout.SerializationTy =
          StructType::create({ArrType}, CInfo.LayoutInfos[LayoutIndex].Name);
    }

    Layout.PayloadMemPointerNode = PayloadMemPointerNode.get();
    Layout.PayloadRootNode = CInfo.PayloadRootNode;
    if (Layout.NodeStorageInfos.count(CInfo.HitAttributeStorageNode))
      Layout.HitAttributeStorageNode = CInfo.HitAttributeStorageNode;
    return Layout;
  }

  Result createResult(const I32Allocation &Allocation) {
    Result Result{};
    Result.MaxNumI32s = Allocation.numUsedI32s();
    Result.Layouts.reserve(CInfo.LayoutInfos.size());
    for (unsigned LayoutIndex = 0; LayoutIndex < CInfo.LayoutInfos.size();
         ++LayoutIndex) {
      Result.Layouts.push_back(
          createSerializationLayout(Allocation, LayoutIndex));
    }
    Result.PayloadMemPointerNode = std::move(PayloadMemPointerNode);
    return Result;
  }

  const CreateInfo &CInfo;
  // Sorted in processing order for the greedy allocation phase
  SmallVector<const NodeInfo *, 16> SortedNodeInfos = {};
  // Is set once we know that a pointer is required
  std::unique_ptr<PAQNode> PayloadMemPointerNode = {};
  std::optional<NodeInfo> PayloadMemPointerNodeInfo = {};
  std::optional<I32Allocation> OptAllocation = {};
}; // namespace

} // namespace

static std::unique_ptr<PAQNode>
createHitAttributeStorageNode(Module &M, uint64_t PayloadHitAttrI32s) {
  assert(PayloadHitAttrI32s && "Attempting to create empty hit attribute node");
  Type *I32 = Type::getInt32Ty(M.getContext());
  Type *I32Arr = ArrayType::get(I32, PayloadHitAttrI32s);
  PAQAccessMask AccessMask;
  AccessMask.set(PAQShaderStage::AnyHit, PAQAccessKind::Write);
  AccessMask.set(PAQShaderStage::AnyHit, PAQAccessKind::Read);
  AccessMask.set(PAQShaderStage::ClosestHit, PAQAccessKind::Read);
  return std::make_unique<PAQNode>(
      PAQNode{I32Arr, {}, AccessMask, lifetimeClassFromAccessMask(AccessMask)});
}

// Table indexed by PAQLifetimeClass containing liveness bitmasks,
// which are indexed by PAQSerializationLayoutKind.
// In other words, the j-th bit in the i-th bitmask specifies whether
// PAQLifetimeClass i is live in PAQSerializationLayoutKind j.
using LivenessBitmaskTable =
    llvm::EnumeratedArray<LayoutBitmask, PAQLifetimeClass,
                          PAQLifetimeClass::Last, std::size_t>;

static const LivenessBitmaskTable &getLivenessBitmaskTable() {
  static const LivenessBitmaskTable LivenessTable = []() {
    LivenessBitmaskTable Initializer = {};
    for (PAQLifetimeClass LifetimeClass : PAQLifetimeClasses) {
      for (PAQSerializationLayoutKind LayoutKind :
           PAQSerializationLayoutKinds) {
        bool IsLive = isLiveInLayout(LifetimeClass, LayoutKind);
        if (IsLive) {
          Initializer[LifetimeClass] |=
              (1u << static_cast<std::size_t>(LayoutKind));
        }
      }
    }
    return Initializer;
  }();

  return LivenessTable;
}

// LayoutComputer wrapper for TraceRay
static LayoutComputer::Result
computeTraceRayLayouts(Module &M, ArrayRef<const PAQNode *> Nodes,
                       const PAQNode *HitAttributesNode,
                       const PAQNode *PayloadRootNode,
                       unsigned PayloadRegisterCount, StringRef NamePrefix) {
  LayoutComputer::CreateInfo LayoutCreateInfo = {M};
  LayoutCreateInfo.LayoutInfos.reserve(PAQSerializationLayoutKinds.size());

  for (auto LayoutKind : PAQSerializationLayoutKinds) {
    std::string TypeName;
    raw_string_ostream TypeNameStream(TypeName);
    TypeNameStream << NamePrefix << "."
                   << determineLayoutSuffix(LayoutKind, {});
    LayoutComputer::LayoutInfo LayoutInfo = {TypeNameStream.str()};
    // We rely on using layout kinds as index into layout infos
    assert(static_cast<std::size_t>(LayoutKind) ==
           LayoutCreateInfo.LayoutInfos.size());
    LayoutCreateInfo.LayoutInfos.push_back({LayoutInfo});
  }

  LayoutCreateInfo.HitAttributeStorageNode = HitAttributesNode;
  LayoutCreateInfo.PayloadRootNode = PayloadRootNode;
  LayoutCreateInfo.PackingOrder = determineLifetimeClassPackingOrder();
  LayoutCreateInfo.PayloadRegisterCount = PayloadRegisterCount;

  const LivenessBitmaskTable &BitmaskByLifetimeClass =
      getLivenessBitmaskTable();

  for (const PAQNode *Node : Nodes) {
    assert(Node);
    LayoutBitmask LivenessBitmask =
        BitmaskByLifetimeClass[Node->LifetimeClass.value()];
    LayoutComputer::NodeInfo NodeInfo = {Node, {}, LivenessBitmask};
    if (Node == HitAttributesNode) {
      // fix hit attribute registers
      assert(Node->Ty->isArrayTy() &&
             Node->Ty->getArrayElementType()->isIntegerTy(32) &&
             "Hit attribute storage must be i32 array!");
      unsigned NumHitAttributeI32s = Node->Ty->getArrayNumElements();
      NodeInfo.FixedIndexIntervals = {
          {FirstPayloadHitAttributeStorageRegister,
           FirstPayloadHitAttributeStorageRegister + NumHitAttributeI32s}};
    }
    LayoutCreateInfo.NodeInfos.push_back(NodeInfo);
  }

  return LayoutComputer::create(LayoutCreateInfo);
}

std::unique_ptr<PAQTraceRaySerializationInfo>
PAQTraceRaySerializationInfo::create(Module &M,
                                     const PAQPayloadConfig &PAQConfig,
                                     const PAQNode &RootNode,
                                     uint64_t PayloadRegisterCount) {
  assert(PAQConfig.PayloadTy == RootNode.Ty);
  std::unique_ptr<PAQTraceRaySerializationInfo> Result =
      std::make_unique<PAQTraceRaySerializationInfo>();
  Result->PayloadRootNode = &RootNode;
  Result->PAQConfig = PAQConfig;

  // Some serialization structs include storage for committed hit attributes.
  // Because we do not know whether intersection shaders are part of the
  // pipeline or not, let alone the maximum size of occurring attribute types,
  // we need to be pessimistic and assume the maximum possible hit attribute
  // size as specified by the app, obtained from
  // PAQConfig.MaxHitAttributeByteCount. SystemData provides some storage for
  // attributes (currently 2 registers), which leaves 6 registers in the payload
  // storage. A whole-pipeline analysis should allow to eliminate these
  // registers, e.g. in case no intersection shaders are present.
  assert(PAQConfig.MaxHitAttributeByteCount <= GlobalMaxHitAttributeBytes);
  const uint32_t MaxInlineHitAttrBytes = getInlineHitAttrsBytes(M);
  const uint32_t InlineHitAttrBytes =
      std::min(MaxInlineHitAttrBytes, PAQConfig.MaxHitAttributeByteCount);
  const uint64_t PayloadHitAttrI32s = divideCeil(
      PAQConfig.MaxHitAttributeByteCount - InlineHitAttrBytes, RegisterBytes);

  if (PayloadHitAttrI32s != 0) {
    // Add node representing hit attribute storage
    Result->MaximumNumHitAttributesI32s = PayloadHitAttrI32s;
    Result->WorstCaseHitAttributesNode =
        createHitAttributeStorageNode(M, PayloadHitAttrI32s);
  }

  // Compute set of individual layouts using LayoutComputer
  SmallVector<const PAQNode *, 16> Nodes;
  Result->collectAllNodes(Nodes);
  std::string NamePrefix = determineSerializationInfoPrefix(PAQConfig);
  LayoutComputer::Result LayoutResult = computeTraceRayLayouts(
      M, Nodes, Result->WorstCaseHitAttributesNode.get(),
      Result->PayloadRootNode, PayloadRegisterCount, NamePrefix);

  // Move layouts to Result, and do dumping and checking
  Result->MaxStorageI32s = LayoutResult.MaxNumI32s;
  // This may be nullptr if registers suffice
  Result->PayloadMemPointerNode = std::move(LayoutResult.PayloadMemPointerNode);
  for (PAQSerializationLayoutKind LayoutKind : PAQSerializationLayoutKinds) {
    Result->LayoutsByKind[LayoutKind] =
        std::move(LayoutResult.Layouts[static_cast<std::size_t>(LayoutKind)]);
    // For lit testing: Dump type information
    LLVM_DEBUG(Result->LayoutsByKind[LayoutKind].print(dbgs(), true));
  }

#ifndef NDEBUG
  checkTraceRaySerializationInfo(*Result, M.getDataLayout());
#endif
  return Result;
}

PAQHitGroupLayoutInfo PAQTraceRaySerializationInfo::createHitGroupLayoutInfo(
    Module &M, uint32_t PayloadHitAttrI32s) const {

  PAQHitGroupLayoutInfo HitGroupLayoutInfo{};
  HitGroupLayoutInfo.NumHitAttributesI32s = PayloadHitAttrI32s;

  if (PayloadHitAttrI32s != 0) {
    // Add node representing hit attribute storage of reduced size
    HitGroupLayoutInfo.HitAttributesNode =
        createHitAttributeStorageNode(M, PayloadHitAttrI32s);
  }

  for (PAQSerializationLayoutKind LayoutKind :
       {PAQSerializationLayoutKind::AnyHitOutAcceptHit,
        PAQSerializationLayoutKind::ClosestHitIn}) {
    const PAQSerializationLayout &DefaultLayout = LayoutsByKind[LayoutKind];

    // Look up storage interval of hit attributes in default layout
    auto It =
        DefaultLayout.NodeStorageInfos.find(WorstCaseHitAttributesNode.get());
    assert(It != DefaultLayout.NodeStorageInfos.end());
    const PAQNodeStorageInfo &HitAtttrsSI = It->second;
    assert(HitAtttrsSI.IndexIntervals.size() == 1 &&
           "Hit attributes must be contiguous!");
    PAQIndexInterval HitAttrInterval = HitAtttrsSI.IndexIntervals[0];
    PAQIndexInterval NewHitAttrInterval = {
        HitAttrInterval.Begin, HitAttrInterval.Begin + PayloadHitAttrI32s};

    // Start with copy, then specialize
    PAQSerializationLayout Layout = DefaultLayout;

    // Update hit attribute index interval and hit attribute node
    Layout.NodeStorageInfos.erase(WorstCaseHitAttributesNode.get());
    if (HitGroupLayoutInfo.HitAttributesNode) {
      Layout.NodeStorageInfos[HitGroupLayoutInfo.HitAttributesNode.get()] = {
          {NewHitAttrInterval}};
      Layout.HitAttributeStorageNode =
          HitGroupLayoutInfo.HitAttributesNode.get();
    } else {
      Layout.HitAttributeStorageNode = nullptr;
    }

    // Recompute storage size by iterating over all storage intervals
    Layout.NumStorageI32s = 0;
    for (const auto &StorageInfo : Layout.NodeStorageInfos) {
      for (const auto &Interval : StorageInfo.second.IndexIntervals) {
        assert(Interval.size() != 0);
        Layout.NumStorageI32s = std::max(Layout.NumStorageI32s, Interval.End);
      }
    }

    // Update type
    if (Layout.NumStorageI32s == 0) {
      Layout.SerializationTy = nullptr;
      assert(Layout.NodeStorageInfos.empty());
    } else {
      std::string NewTypeName;
      raw_string_ostream NewTypeNameStream(NewTypeName);
      NewTypeNameStream << determineSerializationInfoPrefix(PAQConfig) << "."
                        << determineLayoutSuffix(LayoutKind,
                                                 PayloadHitAttrI32s);
      Type *I32 = Type::getInt32Ty(M.getContext());
      ArrayType *ArrType = ArrayType::get(I32, Layout.NumStorageI32s);
      Layout.SerializationTy =
          StructType::create({ArrType}, NewTypeNameStream.str());

      // For lit testing: Dump type information
      LLVM_DEBUG(Layout.print(dbgs(), true));
    }

    // Write to result
    if (LayoutKind == PAQSerializationLayoutKind::AnyHitOutAcceptHit) {
      HitGroupLayoutInfo.AnyHitOutAcceptHitLayout = std::move(Layout);
    } else {
      assert(LayoutKind == PAQSerializationLayoutKind::ClosestHitIn);
      HitGroupLayoutInfo.ClosestHitInLayout = std::move(Layout);
    }
  }

#ifndef NDEBUG
  checkTraceRaySerializationInfo(*this, M.getDataLayout(), &HitGroupLayoutInfo);
#endif
  return HitGroupLayoutInfo;
}

[[maybe_unused]] static void
checkCallShaderSerializationInfo(const PAQCallShaderSerializationInfo &Info,
                                 const DataLayout &DL) {
  checkSerializationLayout(Info.CallShaderSerializationLayout, DL);
}

// LayoutComputer wrapper for CallShader
static LayoutComputer::Result
computeCallShaderLayout(Module &M, ArrayRef<const PAQNode *> Nodes,
                        const PAQNode *PayloadRootNode,
                        unsigned PayloadRegisterCount, StringRef NamePrefix) {
  std::string TypeName;
  raw_string_ostream TypeNameStream(TypeName);
  TypeNameStream << NamePrefix
                 << "."
                 // Indicate CallShader layout by nullopt LayoutKind
                 << determineLayoutSuffix({}, {});
  LayoutComputer::LayoutInfo LayoutInfo = {TypeNameStream.str()};
  LayoutComputer::CreateInfo LayoutCreateInfo = {M};
  LayoutCreateInfo.LayoutInfos = {LayoutInfo};
  LayoutCreateInfo.PayloadRootNode = PayloadRootNode;
  LayoutCreateInfo.PackingOrder = determineLifetimeClassPackingOrder();
  LayoutCreateInfo.PayloadRegisterCount = PayloadRegisterCount;

  for (const PAQNode *Node : Nodes) {
    LayoutCreateInfo.NodeInfos.push_back({Node, {}, 1u});
  }

  return LayoutComputer::create(LayoutCreateInfo);
}

std::unique_ptr<PAQCallShaderSerializationInfo>
PAQCallShaderSerializationInfo::create(Module &M,
                                       const PAQPayloadConfig &PAQConfig,
                                       const PAQNode &PAQRootNode,
                                       uint64_t PayloadRegisterCount) {
  assert(PAQConfig.PayloadTy == PAQRootNode.Ty);
  std::unique_ptr<PAQCallShaderSerializationInfo> Result =
      std::make_unique<PAQCallShaderSerializationInfo>();
  Result->PayloadRootNode = &PAQRootNode;

  SmallVector<const PAQNode *, 16> Nodes;
  Result->collectAllNodes(Nodes);
  std::string NamePrefix = determineSerializationInfoPrefix(PAQConfig);
  LayoutComputer::Result LayoutResult = computeCallShaderLayout(
      M, Nodes, Result->PayloadRootNode, PayloadRegisterCount, NamePrefix);

  // may be nullptr if registers suffice
  Result->PayloadMemPointerNode = std::move(LayoutResult.PayloadMemPointerNode);
  Result->CallShaderSerializationLayout = std::move(LayoutResult.Layouts[0]);
  Result->MaxStorageI32s = LayoutResult.MaxNumI32s;

  StructType *Ty = Result->CallShaderSerializationLayout.SerializationTy;
  if (Ty) {
    // For lit testing: Dump type information
    LLVM_DEBUG(Result->CallShaderSerializationLayout.print(dbgs(), true));
  }

#ifndef NDEBUG
  checkCallShaderSerializationInfo(*Result, M.getDataLayout());
#endif

  return Result;
}

PAQSerializationInfoManager::PAQSerializationInfoManager(
    Module *M, uint32_t MaxPayloadRegCount)
    : Mod{M}, MaxPayloadRegisterCount(MaxPayloadRegCount) {
  TraceRayCache.PAQRootNodes = importModulePayloadPAQNodes(*M);
}

PAQSerializationInfoBase &
PAQSerializationInfoManager::getOrCreateSerializationInfo(
    const PAQPayloadConfig &PayloadConfig, DXILShaderKind ShaderKind) {
  switch (ShaderKind) {
  case DXILShaderKind::RayGeneration:
    llvm_unreachable("RayGen does not have an incoming payload");
  case DXILShaderKind::Intersection:
  case DXILShaderKind::AnyHit:
  case DXILShaderKind::ClosestHit:
  case DXILShaderKind::Miss:
    return getOrCreateTraceRaySerializationInfo(PayloadConfig);
  case DXILShaderKind::Callable:
    return getOrCreateCallShaderSerializationInfo(PayloadConfig);
  default:
    llvm_unreachable("Unexpected DXILShaderKind");
  }
}

PAQTraceRaySerializationInfo &
PAQSerializationInfoManager::getOrCreateTraceRaySerializationInfo(
    const PAQPayloadConfig &PAQConfig) {
  return TraceRayCache.getOrCreateSerializationInfo(
      *Mod, MaxPayloadRegisterCount, PAQConfig);
}

PAQCallShaderSerializationInfo &
PAQSerializationInfoManager::getOrCreateCallShaderSerializationInfo(
    const PAQPayloadConfig &PAQConfig) {
  // Ensure caching doesn't depend on irrelevant fields
  PAQPayloadConfig PAQConfigWithRelevantData = PAQConfig;
  PAQConfigWithRelevantData.MaxHitAttributeByteCount = 0;
  return CallShaderCache.getOrCreateSerializationInfo(
      *Mod, MaxPayloadRegisterCount, PAQConfigWithRelevantData);
}

template <typename SerializationInfoT>
SerializationInfoT &PAQSerializationInfoManager::PAQCache<SerializationInfoT>::
    getOrCreateSerializationInfo(Module &M, uint32_t MaxPayloadRegisterCount,
                                 const PAQPayloadConfig &PAQConfig) {
  auto It = SerializationInfos.find(PAQConfig);
  if (It != SerializationInfos.end())
    return *It->second;

  const PAQNode *PAQRootNode = nullptr;
  auto PAQNodeIt = PAQRootNodes.find(PAQConfig.PayloadTy);
  if (PAQNodeIt != PAQRootNodes.end()) {
    PAQRootNode = PAQNodeIt->second.get();
  } else {
    auto PAQRootNodeUniquePtr =
        createTrivialHierarchicalPayloadRootNode(*PAQConfig.PayloadTy);
    PAQRootNode = PAQRootNodeUniquePtr.get();
    PAQRootNodes.insert({PAQConfig.PayloadTy, std::move(PAQRootNodeUniquePtr)});
  }

  // Compute info
  std::unique_ptr<SerializationInfoT> Info = SerializationInfoT::create(
      M, PAQConfig, *PAQRootNode, MaxPayloadRegisterCount);
  auto InsertionResult =
      SerializationInfos.insert({PAQConfig, std::move(Info)});
  assert(InsertionResult.second && "Unexpected map duplicate!");

  return *InsertionResult.first->second;
}

uint32_t PAQSerializationInfoManager::getMaxPayloadStorageI32s(
    const PAQPayloadConfig &PAQConfig,
    MaxPayloadStorageConsideration Consideration) {
  if (!PAQConfig.PayloadTy)
    return 0;

  uint32_t Result = 0;

  if (Consideration == MaxPayloadStorageConsideration::ConsiderOnlyTraceRay ||
      Consideration ==
          MaxPayloadStorageConsideration::ConsiderTraceRayAndCallShader) {
    Result = std::max(
        Result, getOrCreateTraceRaySerializationInfo(PAQConfig).MaxStorageI32s);
  }

  if (Consideration == MaxPayloadStorageConsideration::ConsiderOnlyCallShader ||
      Consideration ==
          MaxPayloadStorageConsideration::ConsiderTraceRayAndCallShader) {
    Result = std::max(
        Result,
        getOrCreateCallShaderSerializationInfo(PAQConfig).MaxStorageI32s);
  }

  return Result;
}

const PAQSerializationLayout &
PAQSerializationInfoManager::getOrCreateTraceRayLayout(
    PAQTraceRaySerializationInfo &TraceRayInfo,
    PAQSerializationLayoutKind LayoutKind, Type *HitAttributesTy) {

  if (LayoutKind != PAQSerializationLayoutKind::AnyHitOutAcceptHit &&
      LayoutKind != PAQSerializationLayoutKind::ClosestHitIn)
    return TraceRayInfo.LayoutsByKind[LayoutKind];

  // Last case: AnyHitOutAcceptHit or ClosestHitIn layout. Check if
  // HitAttributesTy allows smaller than maximum possible storage. If so, get or
  // create a specialized layout with reduced hit attribute storage size.
  assert(HitAttributesTy && "Hit attributes type required!");

  uint64_t AttrsBytes =
      Mod->getDataLayout().getTypeStoreSize(HitAttributesTy).getFixedValue();
  if (AttrsBytes > TraceRayInfo.PAQConfig.MaxHitAttributeByteCount)
    report_fatal_error("Hit attributes are too large!");
  uint64_t InlineHitAttrsBytes = getInlineHitAttrsBytes(*Mod);
  uint64_t AttrsInPayloadBytes =
      AttrsBytes > InlineHitAttrsBytes ? AttrsBytes - InlineHitAttrsBytes : 0;

  // Number of I32s required in the payload storage
  uint64_t PayloadHitAttrI32s = divideCeil(AttrsInPayloadBytes, RegisterBytes);
  assert(PayloadHitAttrI32s <= TraceRayInfo.MaximumNumHitAttributesI32s &&
         "Hit attributes are too large!");
  if (PayloadHitAttrI32s == TraceRayInfo.MaximumNumHitAttributesI32s) {
    // Hit attributes have maximum size, no need to use specialized layout
    return TraceRayInfo.LayoutsByKind[LayoutKind];
  }

  // Get or create specialized layout
  auto &HitGroupLayouts = TraceRayInfo.SpecializedHitGroupLayouts;
  auto It = HitGroupLayouts.find(PayloadHitAttrI32s);
  if (It == HitGroupLayouts.end()) {
    // Create new specialized hit group layout
    PAQHitGroupLayoutInfo HitGroupLayout =
        TraceRayInfo.createHitGroupLayoutInfo(*Mod, PayloadHitAttrI32s);
    It = HitGroupLayouts.insert({PayloadHitAttrI32s, std::move(HitGroupLayout)})
             .first;
  }
  const PAQHitGroupLayoutInfo &HitGroupLayoutInfo = It->second;
  if (LayoutKind == PAQSerializationLayoutKind::AnyHitOutAcceptHit)
    return HitGroupLayoutInfo.AnyHitOutAcceptHitLayout;

  assert(LayoutKind == PAQSerializationLayoutKind::ClosestHitIn &&
         "Unexpected layout kind!");
  return HitGroupLayoutInfo.ClosestHitInLayout;
}

const PAQSerializationLayout &
PAQSerializationInfoManager::getOrCreateShaderStartSerializationLayout(
    PAQSerializationInfoBase &SerializationInfo, DXILShaderKind ShaderKind,
    Type *HitAttributesTy) {

  assert(ShaderKind != DXILShaderKind::RayGeneration &&
         ShaderKind != DXILShaderKind::Intersection && "Invalid shader kind!");
  if (ShaderKind == DXILShaderKind::Callable)
    return cast<PAQCallShaderSerializationInfo>(SerializationInfo)
        .CallShaderSerializationLayout;

  // Always set for non-intersection
  PAQShaderStage ShaderStage =
      dxilShaderKindToPAQShaderStage(ShaderKind).value();
  // Always set for non-caller, non-intersection read access
  PAQSerializationLayoutKind LayoutKind =
      tryDetermineLayoutKind(ShaderStage, PAQAccessKind::Read).value();
  return getOrCreateTraceRayLayout(
      cast<PAQTraceRaySerializationInfo>(SerializationInfo), LayoutKind,
      HitAttributesTy);
}

const PAQSerializationLayout &
PAQSerializationInfoManager::getOrCreateShaderExitSerializationLayout(
    PAQSerializationInfoBase &SerializationInfo, DXILShaderKind ShaderKind,
    Type *HitAttributesTy, AnyHitExitKind AHExitKind) {

  assert(ShaderKind != DXILShaderKind::RayGeneration &&
         ShaderKind != DXILShaderKind::Intersection && "Invalid shader kind!");
  if (ShaderKind == DXILShaderKind::Callable)
    return cast<PAQCallShaderSerializationInfo>(SerializationInfo)
        .CallShaderSerializationLayout;

  PAQShaderStage ShaderStage =
      dxilShaderKindToPAQShaderStage(ShaderKind).value();
  std::optional<PAQSerializationLayoutKind> OptLayoutKind =
      tryDetermineLayoutKind(ShaderStage, PAQAccessKind::Write);
  if (!OptLayoutKind) {
    // Only for anyhit there are multiple outgoing layout alternatives
    assert(ShaderStage == PAQShaderStage::AnyHit && "Unexpected shader stage!");
    assert(AHExitKind != AnyHitExitKind::None && "Invalid anyhit exit kind!");
    if (AHExitKind == AnyHitExitKind::IgnoreHit) {
      OptLayoutKind = PAQSerializationLayoutKind::AnyHitOutIgnoreHit;
    } else if (AHExitKind == AnyHitExitKind::AcceptHitAndEndSearch) {
      OptLayoutKind =
          PAQSerializationLayoutKind::AnyHitOutAcceptHitAndEndSearch;
    } else {
      assert(AHExitKind == AnyHitExitKind::AcceptHit);
      OptLayoutKind = PAQSerializationLayoutKind::AnyHitOutAcceptHit;
    }
  }
  return getOrCreateTraceRayLayout(
      cast<PAQTraceRaySerializationInfo>(SerializationInfo),
      OptLayoutKind.value(), HitAttributesTy);
}
