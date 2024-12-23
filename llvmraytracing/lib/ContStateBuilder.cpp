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

//===- ContStateBuilder.cpp - A custom ABI for LLVM Coroutines---------------===//
//
// This file defines Continuations Passing Style Return-Continuation ABI for
// LLVM coroutine transforms that is used to build the cont-state.
//===----------------------------------------------------------------------===//

#include "ContStateBuilder.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/IRSerializationUtils.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/TypedPointerType.h"
#include "llvm/Support/OptimizedStructLayout.h"
#include "llvm/Transforms/Coroutines/SpillUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#define DEBUG_TYPE "cont-state-builder"
#define DEBUG_DUMP_CFG(FUNC, MSG)                                                                                      \
  DEBUG_WITH_TYPE("cont-state-cfg-dump", irserializationutils::writeCFGToDotFile(FUNC, MSG))

using namespace llvm;
using namespace llvmraytracing;

static cl::opt<bool> ReportContStateAccessCounts(
    "report-cont-state-access-counts",
    cl::desc("Report on the number of spills (stores) and reloads (loads) from the cont state."), cl::init(false),
    cl::Hidden);

#ifndef NDEBUG
// When debugging a potential issue with the cont-state-builder try setting
// this option to verify the issue resides within the builder.
static cl::opt<bool> UseLLVMContStateBuilder("use-llvm-cont-state-builder",
                                             cl::desc("Use LLVM's built-in continuation state builder."),
                                             cl::init(false), cl::Hidden);
#endif

namespace {

// Representation of a row in the frame-table.
struct CoroFrameRow {
  CoroFrameRow(const DataLayout &DL, Value *D) : Def(D) {
    // Determine alignment of Def
    if (auto *AI = dyn_cast<AllocaInst>(Def)) {
      Ty = AI->getAllocatedType();

      // Make an array type if this is a static array allocation.
      if (AI->isArrayAllocation()) {
        if (auto *CI = dyn_cast<ConstantInt>(AI->getArraySize()))
          Ty = ArrayType::get(Ty, CI->getValue().getZExtValue());
        else
          report_fatal_error("Continuations cannot handle non static allocas yet");
      }
      assert(Ty && "must provide a type for a field");

      // The field size is always the alloc size of the type.
      Size = DL.getTypeAllocSize(Ty);
      assert(Size);

      Alignment = MaybeAlign(AI->getAlign()).value_or(DL.getABITypeAlign(Ty));
      return;
    }

    Ty = Def->getType();

    assert(Ty && "must provide a type for a field");
    Alignment = DL.getABITypeAlign(Ty);

    // The field size is always the alloc size of the type.
    Size = DL.getTypeStoreSize(Ty);
    assert(Size);
  }

  // The original definition of the value or an alloca
  Value *Def = nullptr; // May be an instruction or arg

  // Suspend is in the set if the Def resides in the frame associated with the
  // suspend. The value does not necessarily cross the suspend.
  SmallSet<AnyCoroSuspendInst *, 2> ResidesInSuspendFrame;

  // Offset of value (wrt to this row), in the frame. Note that a value may
  // occupy different parts of a frame if it is respilled. To handle that
  // case one row per frame slot is used. OptimizedStructLayout is used for
  // frame-opt=min-gap so we initialize the value to FlexibleOffset.
  uint64_t Offset = OptimizedStructLayoutField::FlexibleOffset;

  // Alignment is either the type's alignment or the alloca's alignment.
  Align Alignment;

  // Size of field in bytes required for Def
  uint64_t Size = 0;

  // Type of Def, for AllocaInst this is the alloca type
  Type *Ty = nullptr;

  // True if 'spill-on=def' and value has been spilled.
  bool SpilledOnDef = false;
  // Block is in set if value is reloaded there.
  SmallSet<BasicBlock *, 2> ReloadedOnBB;
  // Block is in set if a GEP has been generated for the value there.
  SmallMapVector<BasicBlock *, GetElementPtrInst *, 2> GepInBB;

  // Set of all spill instructions, required for SSA updating.
  SmallVector<StoreInst *, 2> Spills;
  // Set of all reload instructions, required for SSA updating.
  SmallVector<LoadInst *, 2> Reloads;

  // Note: Reloads and spills are added for one suspend at a time. So if it is
  // necessary to know the reloads or spills associated with a given suspend we
  // only need to know their start and end indices within the vectors. We take
  // advantage of this when removing dominate reloads. The start,end pairs per
  // suspend are not currently recorded.

  void dump() const;
};

using CoroFrameTableTy = std::vector<CoroFrameRow>;

struct CoroFrameStruct {
  // Note, although each suspend has a different struct layout only one malloc
  // is done for the coroutine. If fields don't move from suspend to suspend
  // then they don't need to be respilled.

  // Struct layout, optimized by LLVM's OptimizedStructLayout
  SmallVector<OptimizedStructLayoutField, 8> Fields;

  // Alignment of the frame
  Align Alignment;

  // Size of frame in bytes
  uint64_t Size = 0;

  // Suspend and resume BBs
  BasicBlock *SuspendBB;
  BasicBlock *ResumeBB;

  // Crossing values checker
  std::unique_ptr<SuspendCrossingInfo> Checker;

  // SmallMapVector from a spill candidate to a list of its crossing uses.
  coro::SpillInfo CandidateSpills;

  // AllocaInfo includes aliases for crossing allocas.
  SmallVector<coro::AllocaInfo, 8> CandidateAllocas;

  void dumpField(const OptimizedStructLayoutField &F, const CoroFrameTableTy &) const;
  void dump(const CoroFrameTableTy &) const;
};

class ContStateBuilderImpl {
public:
  Function &F;
  coro::Shape &Shape;
  std::function<bool(Instruction &I)> IsMaterializable;

  Module &M;
  const DataLayout &DL;

  ContStateBuilderImpl(Function &F, coro::Shape &S, std::function<bool(Instruction &I)> IsMaterializable)
      : F(F), Shape(S), IsMaterializable(IsMaterializable), M(*F.getParent()), DL(M.getDataLayout()) {}

  // Allocate the coroutine frame and do spill/reload as needed.
  void buildCoroutineFrame();

  // Representation of the combination of all frames, in a table, required for
  // the coroutine.
  CoroFrameTableTy FrameTable;

  // Representation of the frame for the current suspend. If the index is int
  // max then the value is needed and a row will be added to the FrameTable.
  // Otherwise, it is an existing entry in the FrameTable.
  using DefRowMapTy = SmallMapVector<Value *, unsigned, 8>;

  // Value to FrameTable Row map -- used to ensure a value always has the same
  // location in the frame.
  DefRowMapTy AllFrameValues;

  // Map of the optimized struct and fields for each suspend's frame.
  SmallMapVector<AnyCoroSuspendInst *, CoroFrameStruct, 2> FrameStructs;

  // Used to allocate the frame with the size needed to handle the largest
  // computed struct layout and determine if the inline storage is sufficient
  // to hold the frame.
  // Max Frame -- Largest frame required by the suspends.
  // Max Alignment -- Largest individual field's alignment.
  uint64_t MaxFrameSize = 0;
  Align MaxFrameAlign;

  // Helper for building the FrameTable, Count is incremented if a new value is inserted.
  // Returns true if the Def is added, false if it already existed in the FrameTable.
  bool tryInsertFrameTableRow(Value *Def);

  // Go through candidate list and add values that are needed for the suspend
  // to the frame. Note: the location in the frame is not yet finalized.
  void addValuesToFrameTable(AnyCoroSuspendInst *Suspend, const coro::SpillInfo &CandidateSpills,
                             const SmallVector<coro::AllocaInfo, 8> &CandidateAllocas);

  // Make the rows reside in the given suspend's frame
  void makeRowsResideInSuspendFrame(DefRowMapTy &ValueRows, AnyCoroSuspendInst *Suspend);

  // Determine location of gaps in the current frame struct layout.
  void initFrameStructLayout(AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct);

  // Allocate fields according to program order.
  void computeFrameStructLayoutGreedy(AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct, bool IsAlloca);

  // Finalize the struct layout by sorting for spilling and reload, and
  // determining the max frame size and alignments.
  void finalizeFrameStructLayout(CoroFrameStruct &Struct);

  // Create the frame type, its size is the maximum of the frame sizes
  // required at each suspend.
  StructType *createFrameTy() const;

  // In the following spill and reload methods the new insts are added to the
  // insts FrameRow::Reloads and FrameRow::Spills so we can build its phi node
  // network later.

  // Insert spills and reloads
  void insertSpills(coro::Shape &Shape, DominatorTree &DT);
  void insertReloads();

  // With all spills and reloads in-place now we can generate the phi network
  // that carries the values between defs and uses.
  void buildPhiNetwork();

  // Replace poisoned frame-address values with computed values
  void createFrameGEPs(SmallVector<Instruction *, 4> &DeadInstructions);

  // Remove unused reloads
  void removeUnusedReloads();

  // Report stats collected by FrameTable and FrameStruct data structures
  void reportContStateInfo() const;
};

/// Return true if Def is an Arg with the ByVal attribute.
[[maybe_unused]] static bool isArgByVal(Value *Def) {
  if (auto *Arg = dyn_cast<Argument>(Def))
    return Arg->hasByValAttr();
  return false;
}

static std::string getLabel(Function *F) {
  if (F->hasName())
    return F->getName().str();
  ModuleSlotTracker MST(F->getParent());
  MST.incorporateFunction(*F);

  return std::to_string(MST.getLocalSlot(F));
}

static std::string getLabel(BasicBlock *BB) {
  if (BB->hasName())
    return BB->getName().str();

  Function *F = BB->getParent();

  ModuleSlotTracker MST(F->getParent());
  MST.incorporateFunction(*F);

  return std::to_string(MST.getLocalSlot(BB));
}

static std::string getLabel(Value *V) {
  if (V->hasName())
    return V->getName().str();

  if (!isa<Instruction>(V))
    return "";

  BasicBlock *BB = dyn_cast<Instruction>(V)->getParent();
  Function *F = BB->getParent();

  ModuleSlotTracker MST(F->getParent());
  MST.incorporateFunction(*F);

  return std::to_string(MST.getLocalSlot(V));
}

static std::string getAllNames(const SmallSet<BasicBlock *, 2> &List) {
  std::string S;
  if (List.empty())
    return "<empty>";

  for (BasicBlock *BB : List)
    S = S + " %" + getLabel(BB);

  return S;
}

void CoroFrameRow::dump() const {
  if (Def) {
    dbgs() << "\tDef: ";
    LLVM_DEBUG(Def->dump());
    if (isa<Instruction>(Def))
      dbgs() << "\tDefBB: %" << getLabel(cast<Instruction>(Def)->getParent()) << "\n";
    else if (isa<Argument>(Def))
      dbgs() << "\tDefBB: %" << getLabel(cast<Argument>(Def)->getParent()) << "\n";
    else
      dbgs() << "\tDefBB: Unknown Value Type\n";
  } else {
    dbgs() << "\tDef: nullptr\n";
    dbgs() << "\tDefBB: NA\n";
  }
  std::string OffsetStr =
      Offset != OptimizedStructLayoutField::FlexibleOffset ? std::to_string(Offset) : std::string("Flexible");
  dbgs() << "\tOffset: " << OffsetStr << ", " << Size << " bytes, ";
  dbgs() << "Align: " << Alignment.value() << " bytes\n";
  dbgs() << "\tTy: ";
  LLVM_DEBUG(Ty->dump());
  dbgs() << "\tResidesInSuspendFrames: " << ResidesInSuspendFrame.size() << "\n";
  if (!isa<AllocaInst>(Def)) {
    dbgs() << "\tSpilledOnDef: " << (SpilledOnDef ? "true" : "false") << "\n";
    dbgs() << "\tReloadedOnBB: " << getAllNames(ReloadedOnBB) << "\n";
    dbgs() << "\tSpills: " << Spills.size() << "\n";
    dbgs() << "\tReloads: " << Reloads.size() << "\n";
  }
}

void CoroFrameStruct::dumpField(const OptimizedStructLayoutField &F, const CoroFrameTableTy &FrameTable) const {
  auto Idx = reinterpret_cast<long int>(F.Id);
  const CoroFrameRow *Row = &FrameTable[Idx];
  dbgs() << " Frame Table Row " << std::to_string(Idx);
  if (isa<AllocaInst>(Row->Def))
    dbgs() << " -- Alloca for %" << getLabel(Row->Def);
  else if (isa<Argument>(Row->Def))
    dbgs() << " -- Spill of Argument %" << getLabel(Row->Def);
  else
    dbgs() << " -- Spill of Inst %" << getLabel(Row->Def);

  // Determine if value is a spill or alloca
  if (auto *DefAlloca = dyn_cast<AllocaInst>(Row->Def)) {
    auto I = std::find_if(CandidateAllocas.begin(), CandidateAllocas.end(),
                          [DefAlloca](const coro::AllocaInfo &AI) { return AI.Alloca == DefAlloca; });
    if (I == CandidateAllocas.end())
      dbgs() << " -- Unused\n";
    else
      dbgs() << " -- Aliases: " << I->Aliases.size() << "\n";
  } else {
    if (!CandidateSpills.contains(Row->Def))
      dbgs() << " -- Unused\n";
    else
      dbgs() << " -- Crossing Uses: " << CandidateSpills.lookup(Row->Def).size() << "\n";
  }

  if (F.hasFixedOffset()) {
    dbgs() << "\t\tOffset: " << F.Offset << " -> " << F.getEndOffset() << ", " << F.Size << " bytes, ";
    dbgs() << "Align: " << F.Alignment.value() << " bytes\n";
  } else {
    dbgs() << "\t\tOffset: <flexible>\n";
  }
}

void CoroFrameStruct::dump(const CoroFrameTableTy &FrameTable) const {
  dbgs() << "\tFields: \n";
  unsigned idx = 0;
  for (const auto &F : Fields) {
    dbgs() << "\tField " << idx++ << ":";
    dumpField(F, FrameTable);
  }
  dbgs() << "\tFrameStruct Size: " << Size << " bytes, ";
  dbgs() << "Align: " << Alignment.value() << " bytes\n";
  std::string SuspendBBName = SuspendBB ? getLabel(SuspendBB) : "nullptr";
  dbgs() << "\tSuspendBB: %" << SuspendBBName << "\n";
  std::string ResumeBBName = ResumeBB ? getLabel(ResumeBB) : "nullptr";
  dbgs() << "\tResumeBB: %" << ResumeBBName << "\n";
}

bool ContStateBuilderImpl::tryInsertFrameTableRow(Value *Def) {
  auto Idx = FrameTable.size();

  assert(Def);
  auto [Itr, Inserted] = AllFrameValues.try_emplace(Def, Idx);

  if (Inserted) {
    // Add new value
    FrameTable.emplace_back(CoroFrameRow(DL, Def));
  } else {
    // Reuse existing value
    Idx = Itr->second;
  }

  return Inserted;
}

void ContStateBuilderImpl::addValuesToFrameTable(AnyCoroSuspendInst *Suspend, const coro::SpillInfo &CandidateSpills,
                                                 const SmallVector<coro::AllocaInfo, 8> &CandidateAllocas) {
  [[maybe_unused]] unsigned NewArgBytes = 0;
  [[maybe_unused]] unsigned NewInstBytes = 0;
  [[maybe_unused]] unsigned NewAllocaBytes = 0;

  // Add candidate spills. For each suspend that the value crosses it will be
  // added to its frame. The def will be spilled to the frame and a load from
  // the frame will occur before uses where the def-use crosses the suspend.
  for (auto &[Def, Aliases] : CandidateSpills) {
    if (tryInsertFrameTableRow(Def)) {
      // Statistics collection
      LLVM_DEBUG({
        auto Idx = AllFrameValues.lookup(Def);
        auto &Row = FrameTable[Idx];
        if (isArgByVal(Def))
          llvm_unreachable("ByVal Args are unsupported");
        else if (isa<Argument>(Def))
          NewArgBytes += Row.Size;
        else
          NewInstBytes += Row.Size;
      });
    }
  }

  for (auto &AI : CandidateAllocas) {
    // Note: CandidateAllocas have already been determined to cross a suspend.
    // We can also assume that sinkSpillUsesAfterCoroBegin moved all uses to
    // after the CoroBegin.

    if (tryInsertFrameTableRow(AI.Alloca)) {
      // Statistics collection
      LLVM_DEBUG({
        auto Idx = AllFrameValues.lookup(AI.Alloca);
        auto &Row = FrameTable[Idx];
        NewAllocaBytes += Row.Size;
      });
    }
  }

  LLVM_DEBUG({
    dbgs() << "\tNew Alloca Bytes: " << NewAllocaBytes << "\n";
    dbgs() << "\tNew Arg Spill Bytes: " << NewArgBytes << "\n";
    dbgs() << "\tNew Inst Spill Bytes: " << NewInstBytes << "\n";
  });

  // Adding AllFrameValues rows to the given suspend's frame will prevent
  // values that are no longer needed from being overwritten.
  makeRowsResideInSuspendFrame(AllFrameValues, Suspend);
}

void ContStateBuilderImpl::makeRowsResideInSuspendFrame(DefRowMapTy &FrameValues, AnyCoroSuspendInst *Suspend) {
  // Add this Suspend point to ResidesInSuspendFrame for all frame rows.
  for (const auto &[Def, Idx] : FrameValues) {
    auto &Row = FrameTable[Idx];
    Row.ResidesInSuspendFrame.insert(Suspend);
  }
}

// Check if Def value crosses the suspend. Note this check is used instead of
// checking the ResidesInSuspendFrame set because if eviction is not enabled
// then the ResidesInSuspendFrame set will include all suspends.
static bool isSuspendCrossingValue(Value *Def, const coro::SpillInfo &CandidateSpills,
                                   const SmallVector<coro::AllocaInfo, 8> &CandidateAllocas) {
  if (auto *DefAlloca = dyn_cast<AllocaInst>(Def)) {
    auto II = std::find_if(CandidateAllocas.begin(), CandidateAllocas.end(),
                           [DefAlloca](const coro::AllocaInfo &AI) { return AI.Alloca == DefAlloca; });
    return II != CandidateAllocas.end();
  }

  return CandidateSpills.contains(Def);
}

static void fitNewField(Value *Val, OptimizedStructLayoutField &NewField, CoroFrameStruct &Struct) {
  NewField.Offset = 0; // If Fields is empty, start at offset 0
  if (!Struct.Fields.empty()) {
    NewField.Offset = alignTo(Struct.Size, NewField.Alignment);
    assert(NewField.Offset >= Struct.Size);
  }

  Struct.Size = NewField.getEndOffset();

  if (Struct.Alignment < NewField.Alignment)
    Struct.Alignment = NewField.Alignment;

  Struct.Fields.emplace_back(NewField);
}

void ContStateBuilderImpl::initFrameStructLayout(AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct) {
  assert(Struct.Fields.empty());

  // First add fields that have already been located (fixed offset fields).
  for (auto R : llvm::enumerate(FrameTable)) {
    auto &Row = R.value();
    // Notice we include all values that have this Suspend in their
    // ResidesInSuspendFrame set. That will ensure all values currently held in
    // the frame will be added as a Field. If eviction is enabled that set will
    // only include values that are used across the suspend.
    if (Row.Offset != OptimizedStructLayoutField::FlexibleOffset && Row.ResidesInSuspendFrame.contains(Suspend)) {
      // Value is in this frame, create a 'field' for it.
      void *Idx = reinterpret_cast<void *>(R.index());
      Struct.Fields.emplace_back(Idx, Row.Size, Row.Alignment, Row.Offset);
    }
  }

  // Sort the fixed offset fields to identify gaps between existing values.
  llvm::sort(
      Struct.Fields.begin(), Struct.Fields.end(),
      [&](const OptimizedStructLayoutField &A, const OptimizedStructLayoutField &B) { return A.Offset < B.Offset; });

  // After sorting last element in Fields is the last in memory.
  if (!Struct.Fields.empty())
    Struct.Size = Struct.Fields.back().getEndOffset();
}

void ContStateBuilderImpl::computeFrameStructLayoutGreedy(AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct,
                                                          bool IsAlloca) {
  // Add flexible fields into the gaps
  for (auto R : llvm::enumerate(FrameTable)) {
    auto &Row = R.value();
    // Only layout non-alloca, skip alloca - they are laid out separately
    if (!IsAlloca && isa<AllocaInst>(Row.Def))
      continue;

    // Only layout alloca, skip non-alloca - they are laid out separately
    if (IsAlloca && !isa<AllocaInst>(Row.Def))
      continue;

    if (Row.Offset == OptimizedStructLayoutField::FlexibleOffset && Row.ResidesInSuspendFrame.contains(Suspend)) {
      // Value is in this frame, create a 'field' for it.
      void *Idx = reinterpret_cast<void *>(R.index());
      OptimizedStructLayoutField NewField = {Idx, Row.Size, Row.Alignment, Row.Offset};
      fitNewField(Row.Def, NewField, Struct);
      assert(NewField.Offset != OptimizedStructLayoutField::FlexibleOffset);
      assert(NewField.Offset == alignTo(NewField.Offset, NewField.Alignment));

      // Update the offsets in the FrameTable
      Row.Offset = NewField.Offset;
    }
  }
}

void ContStateBuilderImpl::finalizeFrameStructLayout(CoroFrameStruct &Struct) {
  // Sort the fields so spills and reloads are created in sequenced such that
  // their offsets are in increasing order.
  llvm::sort(
      Struct.Fields.begin(), Struct.Fields.end(),
      [&](const OptimizedStructLayoutField &A, const OptimizedStructLayoutField &B) { return A.Offset < B.Offset; });

  assert(Struct.Fields.empty() || Struct.Fields.back().getEndOffset() == Struct.Size);

  // Record the largest frame required by the coroutine
  if (MaxFrameSize < Struct.Size)
    MaxFrameSize = Struct.Size;

  if (MaxFrameAlign < Struct.Alignment)
    MaxFrameAlign = Struct.Alignment;
}

StructType *ContStateBuilderImpl::createFrameTy() const {
  // TODO - when allocating the array (by user) the alignment may need to be
  // corrected, this can be done by over-allocating e.g. size+alignment-1,
  // then offsetting the start ptr to correct the alignment.

  LLVMContext &C = F.getContext();

  // Create a structure -- LLVM's CoroFrame builds a real struct with types
  // that match the values for its frame. Here we build a struct with a sized
  // array and index into that using the provided offsets. We do this for
  // several reasons:
  // 1) At each suspend we want the frame to have only the required fields,
  //    unused fields should be allowed to be overwritten by any other field,
  //    no matter if the types match. However, typed struct fields make this
  //    more difficult, potentially requiring a different struct type per
  //    suspend point.
  // 2) Notice that offsets into the frame are computed first (above) then
  //    the frame type is created. LLVM's CoroFrame then builds a struct with
  //    typed fields. However, the struct type layout is a different method
  //    than the struct field optimizer and thus may have a different padding
  //    between fields. This could introduce alignment errors and
  //    out-of-bounds accesses.
  // 3) It is necessary to add padding to the struct type to avoid the above
  //    fragility, however, that changes the index of the fields. This must be
  //    tracked and is another potential point of failure.
  // 4) The array is wrapped in a struct so it can be given a name, otherwise
  //    it is not possible to give a stand-alone array type a name.
  //
  auto Name = F.getName() + ".Frame";

  Type *ByteArray = ArrayType::get(Type::getInt8Ty(C), MaxFrameSize);
  StructType *FrameType = StructType::create(C, {ByteArray}, Name.str());

  // Verify the struct type is the right size, i.e. no padding was added.
  assert(DL.getTypeAllocSize(FrameType) == MaxFrameSize);

  return FrameType;
}

void ContStateBuilderImpl::insertSpills(coro::Shape &Shape, DominatorTree &DT) {
  LLVMContext &C = F.getContext();
  IRBuilder<> Builder(C);

  // Determine if the spill is needed for this def and set the insertion pt.
  auto SetInsertPtIfRequired = [&](CoroFrameRow &Row) {
    if (!Row.SpilledOnDef) {
      auto I = coro::getSpillInsertionPt(Shape, Row.Def, DT);
      Builder.SetInsertPoint(I);
      Row.SpilledOnDef = true;
      return true;
    }

    return false;
  };

  for (auto &I : FrameStructs) {
    auto &Struct = I.second;

    // For each value in the frame insert spill, if they do not already exist.
    // Note: the location in the frame will be set when GEPs are built later
    // for now the addresses are poisoned.

    // Visit each field in the struct and create spills as needed. Visit fields
    // in reverse order to cause the spills to occur in-order after creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<long int>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];
      Value *Def = Row.Def;

      // Allocas in the frame do not require spilling.
      if (isa<AllocaInst>(Def))
        continue;

      // Do not spill here if the value does not cross this suspend. Note
      // this check is needed when eviction is not used. Without eviction
      // the frame will include values that do not cross it and we should
      // not spill the value on suspends the value does not cross. That
      // will lead to excess spilling and incorrect codegen.
      if (!isSuspendCrossingValue(Row.Def, Struct.CandidateSpills, Struct.CandidateAllocas))
        continue;

      if (!SetInsertPtIfRequired(Row))
        continue;

      // Generate a frame address of the Def, poison for now
      Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

      // Generate spill for Def
      StoreInst *Spill = Builder.CreateAlignedStore(Def, PoisonFrameAddr, Row.Alignment);

      // Record spill so we can build the phi node network and fix the frame
      // address later.
      assert(Spill);
      Row.Spills.emplace_back(Spill);
    }
  }
}

void ContStateBuilderImpl::insertReloads() {
  LLVMContext &C = F.getContext();
  IRBuilder<> Builder(C);

  // Determine if a reload is needed for this use and set the insertion pt.
  auto SetInsertPtIfRequired = [&](CoroFrameRow &Row, User *U) {
    auto *UseBB = cast<Instruction>(U)->getParent();

    Builder.SetInsertPoint(UseBB, UseBB->getFirstInsertionPt());
    // Mark the reloaded BB so we don't reload it a second time
    auto R = Row.ReloadedOnBB.insert(UseBB);
    return R.second; // False if UseBB already existed in the set.
  };

  // Generate a frame address of the Def, poison for now.
  Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

  for (auto &I : FrameStructs) {
    auto &Struct = I.second;

    // For each value in the frame insert reloads, if they do not already
    // exist. Note: the location in the frame will be set when GEPs are built
    // later for now the addresses are poisoned. Note: not all uses of the
    // value can be set because the phi node network that connects the new defs
    // must be created.

    // Visit each field in the struct and create reloads as needed. Visit the
    // in reverse order to cause the reloads to occur in-order after creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<long int>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];
      Value *Def = Row.Def;

      // Allocas in the frame do not require reloading
      if (isa<AllocaInst>(Def))
        continue;

      // Do not reload here if the value does not cross this suspend. Note this
      // check is needed when eviction is not used. Without eviction the frame
      // will include values that do not cross it and we should not reload the
      // value on suspends the value does not cross. That will lead to excess
      // reloading and incorrect codegen.
      if (!isSuspendCrossingValue(Row.Def, Struct.CandidateSpills, Struct.CandidateAllocas))
        continue;

      auto &SpillUses = Struct.CandidateSpills[Def];

      // Helper to connect a reload to the uses of this Def if the use and the
      // reload are in the same BB.
      auto ConnectReloadToUses = [&](LoadInst *Reload) {
        for (auto *U : SpillUses) {
          // If the Reload and the Use are in the same BB then relink the Use to
          // the Reload. This is done here because SSA Updater cannot easily
          // produce a value if the Use is in the same BB. This is also a compiler-
          // time optimization because it eliminates the need to invoke SSA Updater
          // for this Use.
          auto *Inst = cast<Instruction>(U);
          if (Reload->getParent() == Inst->getParent() && !isa<PHINode>(U)) {
            Inst->replaceUsesOfWith(Def, Reload);
          }
        }
      };

      // If we didn't generate a reload-on-resume then try to generate reloads
      // on (near) each use.
      for (auto *U : SpillUses) {
        if (!SetInsertPtIfRequired(Row, U))
          continue;

        // Generate reload for Def
        auto *CurrentReload = Builder.CreateAlignedLoad(Row.Ty, PoisonFrameAddr, Row.Alignment,
                                                        Twine("reload.row") + std::to_string(Idx) + Twine(".") +
                                                            Row.Def->getName() + Twine("."));

        // Record the reload so we can build the phi node network and fix the frame
        // address later.
        Row.Reloads.emplace_back(CurrentReload);
      }

      // Connect immediate uses
      for (auto &Reload : Row.Reloads) {
        ConnectReloadToUses(Reload);
      }
    }
  }
}

void ContStateBuilderImpl::buildPhiNetwork() {
  LLVMContext &C = F.getContext();
  [[maybe_unused]] Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

  // For each value collect all defs and reloads (available values)
  // Then go back and fix up all spills and uses using SSA Updater.
  for (CoroFrameRow &Row : FrameTable) {
    // We don't need to build the phi node network for allocas because their
    // loads already inserted by the user.
    if (isa<AllocaInst>(Row.Def))
      continue;

    // Setup the SSAUpdater
    SSAUpdater Updater;
    Updater.Initialize(Row.Ty, Row.Def->getName());

    // Add the original def and the materialized defs so SSAUpdater has all
    // available definitions of the value.
    if (auto *OldInst = dyn_cast<Instruction>(Row.Def))
      Updater.AddAvailableValue(OldInst->getParent(), OldInst);
    else if (auto *OldArg = dyn_cast<Argument>(Row.Def))
      Updater.AddAvailableValue(&OldArg->getParent()->getEntryBlock(), OldArg);
    else
      llvm_unreachable("Unhandled type");

    // Reloads are new definitions of the same value
    for (LoadInst *ReloadInst : Row.Reloads)
      Updater.AddAvailableValue(ReloadInst->getParent(), ReloadInst);

    // Copy because GetValueAtEndOfBlock will introduce additional users of
    // the def (PHINodes).
    SmallVector<User *, 2> DefUsers(Row.Def->users());

    // All users of Def are visited here to ensure all SSA uses have a proper
    // phi node network connecting it to the nearest def/reload.

    // This case is rather simple, because we know the value must cross a
    // suspend, and all remats should be done either on resume or right before
    // any uses of old def so we can assume the value should be live-out.
    for (User *U : DefUsers) {
      auto *DefUse = cast<Instruction>(U);
      auto *DefUseBB = DefUse->getParent();

      // Check that the user is not a spill that we inserted.
      if (auto *DefUseSI = dyn_cast<StoreInst>(DefUse)) {
        auto It = std::find(Row.Spills.begin(), Row.Spills.end(), DefUseSI);
        // If the DefUse is a spill we inserted, skip it, we already hooked it up.
        if (It != Row.Spills.end()) {
          // Our spills have a poison address at this point.
          assert(DefUseSI->getPointerOperand() == PoisonFrameAddr);
          continue;
        }
      }

      // If the user is a PHI node, it should be a single-edge phi node and we
      // can replace its uses with the new definition.
      if (auto *PN = dyn_cast<PHINode>(DefUse)) {
        assert(PN->getNumIncomingValues() == 1 && "unexpected number of incoming "
                                                  "values in the PHINode");

        if (!PN->use_empty()) {
          Value *NewDef = Updater.GetValueAtEndOfBlock(DefUseBB);
          PN->replaceAllUsesWith(NewDef);
        }

        // Now the phi node is dead
        PN->eraseFromParent();
        continue;
      }

      // For non phi-nodes we replace the uses of the old def with the new def.
      Value *NewDef = nullptr;
      for (unsigned i = 0, E = DefUse->getNumOperands(); i != E; ++i) {
        if (DefUse->getOperand(i) == Row.Def) {
          if (!NewDef)
            NewDef = Updater.GetValueAtEndOfBlock(DefUseBB);
          DefUse->setOperand(i, NewDef);
        }
      }
    }
  }
}

void ContStateBuilderImpl::createFrameGEPs(SmallVector<Instruction *, 4> &DeadInstructions) {
  LLVMContext &C = F.getContext();
  IRBuilder<NoFolder> Builder(C);

  // Replace the poison on the spills and reloads with GEPs into the frame.
  Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

  // Insertion point for GEP that replaces alloca
  BasicBlock *FramePtrBB = Shape.getInsertPtAfterFramePtr()->getParent();

  // Split the FramePtrBB to add a 'spill' block immediately following the
  // frame ptr.
  auto SpillBlock = FramePtrBB->splitBasicBlock(Shape.getInsertPtAfterFramePtr(), Twine("AllocaSpillBB"));
  SpillBlock->splitBasicBlock(&SpillBlock->front(), Twine("PostSpill.") + FramePtrBB->getName());
  Shape.AllocaSpillBlock = SpillBlock;

  // Each suspend corresponds to a potentially unique frame
  for (auto &I : FrameStructs) {
    [[maybe_unused]] auto *Suspend = I.first;
    auto &Struct = I.second;
    // Visit each field in the struct and create reloads as needed. Visit the
    // fields in reverse order to cause the reloads to occur in-order after
    // creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<long int>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];

      assert(Row.ResidesInSuspendFrame.contains(Suspend));
      assert(Row.Offset != OptimizedStructLayoutField::FlexibleOffset);

      auto TryReuseGep = [&](BasicBlock *BB, BasicBlock::iterator InsertPt, const Twine &Label, StringRef Name) {
        auto [Itr, Inserted] = Row.GepInBB.try_emplace(BB, nullptr);

        // Add a new GEP if the BB is not in the map
        if (!Inserted) {
          // Get GEP from map
          assert(Itr->second);
          return Itr->second;
        }

        // Set the insert pt of the GEP
        Builder.SetInsertPoint(InsertPt);

        // FrameTy is a struct containing an array of int8, i.e.
        //  struct value_frame { char data[size]; };
        // FramePtr will be replaced by an alloca of the right size, i.e.
        // Accesses to the frame will look like
        //  v->data[Row.Offset];
        // So this translates to indices {
        //  0,  <- frame ptr is not an array, we don't index into it
        //  0,  <- accessing the first member (data) in the struct
        //  Row.Offset <- accessing an element of the data array
        // }
        Value *Idxs[] = {ConstantInt::get(Type::getInt32Ty(C), 0), ConstantInt::get(Type::getInt32Ty(C), 0),
                         ConstantInt::get(Type::getInt32Ty(C), Row.Offset)};

        // GEP replacing alloca
        auto *GepInst = Builder.CreateInBoundsGEP(Shape.FrameTy, Shape.FramePtr, Idxs,
                                                  Label + Twine(".addr.row") + std::to_string(Idx) + Twine(".") + Name +
                                                      Twine("."));
        Itr->second = dyn_cast<GetElementPtrInst>(GepInst);

        return Itr->second;
      };

      // Fix allocas that are taken over by the frame. Note that allocas that
      // do not cross suspends are not included in the FrameTable.
      if (auto *Alloca = dyn_cast<AllocaInst>(Row.Def)) {
        // Insert a GEP to replace the alloca immediately after the malloc of
        // the coro frame to ensure all accesses are dominated by the GEP.
        // Insert at the end of the spill block.
        auto *GepInst =
            TryReuseGep(SpillBlock, SpillBlock->getTerminator()->getIterator(), Twine("alloca"), Alloca->getName());

        // Note: that the location of the GEP is not be the same as that of
        // the alloca. The GEP is put into the SpillBlock. The SpillBlock is
        // the entry point of each continuation, so any instrs put there will
        // be available to all continuations after the main function is split.
        CompilerUtils::replaceAllPointerUses(Alloca, GepInst, DeadInstructions);

        // Alloca is dead, we may visit this Row more than once, so we need to
        // check if the value is in the DeadInstructions list already.
        if (std::find(DeadInstructions.begin(), DeadInstructions.end(), Alloca) == DeadInstructions.end()) {
          // Insert the AllocaInst if it's not a duplicate
          DeadInstructions.push_back(Alloca);
        }

        continue; // Alloca do not have Spills or Reloads
      }

      // Fix spill (store) address
      for (StoreInst *SpillInst : Row.Spills) {
        auto *SpillBB = SpillInst->getParent();

        // Set insertion point before the SpillInst
        auto *GepInst =
            TryReuseGep(SpillBB, SpillInst->getParent()->getFirstInsertionPt(), Twine("frame"), Row.Def->getName());

        // Replace the SpillInst ptr, that is Poison, with the GEP.
        if (SpillInst->getPointerOperand() == PoisonFrameAddr)
          SpillInst->setOperand(1, GepInst);
      }

      // Fix reload (load) address
      for (LoadInst *ReloadInst : Row.Reloads) {
        auto *ReloadBB = ReloadInst->getParent();

        // Set insertion point before the ReloadInst
        auto *GepInst =
            TryReuseGep(ReloadBB, ReloadInst->getParent()->getFirstInsertionPt(), Twine("frame"), Row.Def->getName());

        // Replace the ReloadInst ptr, that is Poison, with the GEP.
        if (ReloadInst->getPointerOperand() == PoisonFrameAddr)
          ReloadInst->setOperand(0, GepInst);
      }
    }
  }
}

void ContStateBuilderImpl::removeUnusedReloads() {
  for (auto &Row : FrameTable) {
    // There should be 1 reload per BB where a reload occurs
    assert(Row.Reloads.size() == Row.ReloadedOnBB.size());

    SmallVector<LoadInst *, 2> UsedReloads;

    // Identify the used reloads and keep them, remove the unused ones.
    for (LoadInst *R : Row.Reloads) {

      if (!R->use_empty()) {
        UsedReloads.push_back(R);
        continue;
      }

      assert(R->use_empty() && R->materialized_use_empty());

      // This is an unused reload, remove it.
      Row.ReloadedOnBB.erase(R->getParent());

      // Remove reload
      R->eraseFromParent();
    }

    // Now remove the old reloads list.
    Row.Reloads = UsedReloads;

    LLVM_DEBUG({
      for (LoadInst *R : Row.Reloads)
        assert(!R->use_empty());
    });

    // There should be 1 reload per BB where a reload occurs
    assert(Row.Reloads.size() == Row.ReloadedOnBB.size());
  }
}

static bool hasPoisonOperand(Instruction *I) {
  // Check GetElementPtrInst
  if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
    for (auto &Op : GEP->operands())
      if (isa<PoisonValue>(Op))
        return true;
  }
  // Check LoadInst
  else if (auto *LI = dyn_cast<LoadInst>(I)) {
    if (isa<PoisonValue>(LI->getPointerOperand()))
      return true;
  }
  // Check StoreInst
  else if (auto *SI = dyn_cast<StoreInst>(I)) {
    if (isa<PoisonValue>(SI->getPointerOperand()) || isa<PoisonValue>(SI->getValueOperand()))
      return true;
  }
  // Check PHINode
  else if (auto *PN = dyn_cast<PHINode>(I)) {
    for (auto &Op : PN->operands())
      if (isa<PoisonValue>(Op))
        return true;
  }

  return false;
}

[[maybe_unused]] static void collectInstWithPoison(Function &F, SmallSet<Instruction *, 16> &PoisonInstructions) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      // Record the instruction if it has a poison operand
      if (hasPoisonOperand(&I)) {
        PoisonInstructions.insert(&I);
      }
    }
  }
}

[[maybe_unused]] static bool hasNewPoisonOperand(Function &F, const SmallSet<Instruction *, 16> &PoisonInstructions) {
  bool foundNewPoison = false;

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (PoisonInstructions.count(&I) > 0)
        continue;

      // If a new poison operand is found, dump the instruction and set the flag
      if (hasPoisonOperand(&I)) {
        errs() << "Found poison operand in instruction: " << I << "\n";
        foundNewPoison = true;
      }
    }
  }

  return foundNewPoison;
}

void ContStateBuilderImpl::reportContStateInfo() const {
  uint64_t TotalReloads = 0;
  uint64_t TotalSpills = 0;
  uint64_t TotalGeps = 0;

  for (auto &Row : FrameTable) {
    if (isa<AllocaInst>(Row.Def))
      continue;

    TotalGeps += Row.GepInBB.size();
    TotalReloads += Row.Reloads.size();
    TotalSpills++;

    for (auto &I : FrameStructs) {
      auto &Struct = I.second;
      if (!Struct.CandidateSpills.contains(Row.Def))
        continue;
    }
  }

  // Note, these stats should closely match the stats reported by
  // reportGepsSpillsAndReloads that counts raw geps, reloads and spills
  // before and after building the cont state.
  dbgs() << "Final # of Geps: " << TotalGeps << "\n";
  dbgs() << "Final # of Reloads: " << TotalReloads << "\n";
  dbgs() << "Final # of Spills: " << TotalSpills << "\n";
}

template <typename InstType> static unsigned countInstrs(const Function &F) {
  unsigned Total = 0;
  for (auto &BB : F)
    for (auto &I : BB)
      if (isa<InstType>(&I))
        Total++;
  return Total;
}

// Report absolute number of new geps, spills and reloads inserted by the
// continuation state builder.
static void reportGepsSpillsAndReloads(Function &F, unsigned NonFrameGeps, unsigned NonSpillStores,
                                       unsigned NonReloadLoads) {
  if (ReportContStateAccessCounts) {
    unsigned FrameGeps = countInstrs<GetElementPtrInst>(F);
    assert(FrameGeps >= NonFrameGeps);
    FrameGeps -= NonFrameGeps;

    unsigned SpillStores = countInstrs<StoreInst>(F);
    assert(SpillStores >= NonSpillStores);
    SpillStores -= NonSpillStores;

    unsigned ReloadLoads = countInstrs<LoadInst>(F);
    assert(ReloadLoads >= NonReloadLoads);
    ReloadLoads -= NonReloadLoads;

    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    dbgs() << "Continuation state geps of \"" << F.getName() << "\" (" << Stage << "): " << FrameGeps << "\n";
    dbgs() << "Continuation state reloads of \"" << F.getName() << "\" (" << Stage << "): " << ReloadLoads << "\n";
    dbgs() << "Continuation state spills of \"" << F.getName() << "\" (" << Stage << "): " << SpillStores << "\n";
  }
}

void ContStateBuilderImpl::buildCoroutineFrame() {
  // This method builds a unique frame for each suspend point. The frame
  // includes values that are needed for the resume.
  //
  // The spills and reloads are inserted with poison addresses. These addresses
  // are set to real frame addresses after all spills and reloads for all
  // frames have been identified and inserted. This makes it easier to both
  // optimize the frame layout and optimize the location of spills and reloads
  // without worrying about how to get the right frame addresses. Similarly,
  // the spilled values and the uses of the reloaded values are also set after
  // all spills and reloads have been inserted. This allows us to use SSA
  // Updater to build the phi node networks when necessary.

  // ======== Do Rematerializations ========

  LLVM_DEBUG(dbgs() << "Running Rematerialization\n");

  // For default remat we need to do that before spilling
  SuspendCrossingInfo FullChecker(F, Shape.CoroSuspends, Shape.CoroEnds);
  coro::doRematerializations(F, FullChecker, IsMaterializable);

  // ======== Initial Load and Store Stats ========

  unsigned NonFrameGeps = countInstrs<GetElementPtrInst>(F);
  unsigned NonSpillStores = countInstrs<StoreInst>(F);
  unsigned NonReloadLoads = countInstrs<LoadInst>(F);

  // ======== Create a frame struct per suspend ========

  LLVM_DEBUG(dbgs() << "Running SuspendCrossingInfo Analysis\n");

  for (auto *Suspend : Shape.CoroSuspends) {
    // Create a frame struct per suspend
    auto &Struct = FrameStructs[Suspend];
    Struct.Checker =
        std::make_unique<SuspendCrossingInfo>(F, SmallVector<AnyCoroSuspendInst *>({Suspend}), Shape.CoroEnds);

    // Normalization already splits the BB around the suspend instructions.
    BasicBlock *BB = Suspend->getParent();
    Struct.SuspendBB = BB->getSinglePredecessor();
    Struct.ResumeBB = BB->getSingleSuccessor();
  }

  // Renumber the blocks, normalization will have inserted new blocks.
  F.renumberBlocks();
  DominatorTree DT(F);

  SmallVector<Instruction *, 4> DeadInstructions;
  SmallVector<CoroAllocaAllocInst *, 4> LocalAllocas;
  // Note: CoroAlloca* are used by swift, we don't need to handle them.

  DEBUG_DUMP_CFG(F, "pre-frame-build-cfg");

  // ======== Gather candidate spills and allocas ========

  LLVM_DEBUG(dbgs() << "Gathering Spills and Allocas\n");

  for (auto *Suspend : Shape.CoroSuspends) {
    // Create a frame struct per suspend
    auto &Struct = FrameStructs[Suspend];

    assert(Struct.CandidateSpills.empty());
    assert(Struct.CandidateAllocas.empty());

    // Collect the candidate spills for arguments and other not-materializable
    // values for this suspend.
    coro::collectSpillsFromArgs(Struct.CandidateSpills, F, *Struct.Checker);
    coro::collectSpillsAndAllocasFromInsts(Struct.CandidateSpills, Struct.CandidateAllocas, DeadInstructions,
                                           LocalAllocas, F, *Struct.Checker, DT, Shape);
  }

  // ======== Frame Layout ========

  auto Id = Shape.getRetconCoroId();
  auto RetconSize = Id->getStorageSize();
  auto RetconAlign = Id->getStorageAlignment();

  LLVM_DEBUG({
    dbgs() << "----- Frame Data At Each Suspend -----\n";
    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    dbgs() << "Function: " << F.getName() << " (" << Stage << ")\n";
    dbgs() << "Total # of Suspends: " << FrameStructs.size() << "\n";
  });

  for (auto R : llvm::enumerate(FrameStructs)) {
    auto *Suspend = R.value().first;
    auto &Struct = R.value().second;
    LLVM_DEBUG(dbgs() << "Suspend " << R.index() << "\n");
    LLVM_DEBUG(dbgs() << "\tSuspendInst: "; Suspend->dump());
    LLVM_DEBUG(dbgs() << "\tSuspendBB: %" << getLabel(Suspend->getParent()) << "\n");

    // Sink spill uses. This will move all uses of allocas to after the
    // CoroBegin ensuring that all access to the alloca ptr occur after
    // the Coro frame ptr has been malloced by the user code. This simplifies
    // handling alloca because it means we can simply replace the alloca with
    // space on the frame. So there are two cases: the alloca does not cross a
    // suspend so we leave it alone, or the alloca crosses a suspend so we put
    // it into the coroutine frame.
    coro::sinkSpillUsesAfterCoroBegin(DT, Shape.CoroBegin, Struct.CandidateSpills, Struct.CandidateAllocas);

    // Go through candidate list and add values that are needed for this
    // suspend. Note: the offset into the frame is not yet finalized.
    addValuesToFrameTable(Suspend, Struct.CandidateSpills, Struct.CandidateAllocas);

    // Initialize the fields with pre-existing offsets.
    initFrameStructLayout(Suspend, Struct);

    // Compute greedy struct layout of alloca. This places alloca first in the
    // frame struct, before non-alloca values.
    computeFrameStructLayoutGreedy(Suspend, Struct, /*IsAlloca*/ true);

    // Next, compute greedy struct layout of non-alloca.
    computeFrameStructLayoutGreedy(Suspend, Struct, /*IsAlloca*/ false);

    // Sorting fields by offset and determine the total frame size required.
    finalizeFrameStructLayout(Struct);

    LLVM_DEBUG({
      dbgs() << "\tFrame Size Bytes: " << Struct.Size << "\n";
      dbgs() << "\tFrame Align Bytes: " << Struct.Alignment.value() << "\n";
    });
  }

  // Create the Shape.FrameTy, the maximum of the frame sizes computed above
  Shape.FrameTy = createFrameTy();

  // CoroSplit will replace any uses of CoroBegin with an alloca (or similar).
  // So where we need the frame ptr we just use CoroBegin.
  Shape.FramePtr = Shape.CoroBegin;

  // IsFrameInlineInStorage determines if split coroutines will malloc a new
  // frame. Typically this is done because the default frame provided by
  // coro.id is not large enough. That would be done with this logic:
  Shape.RetconLowering.IsFrameInlineInStorage = (MaxFrameSize <= RetconSize && MaxFrameAlign <= RetconAlign);
  // However, we may elict to never use the inline storage to avoid the special
  // cases it requires.

  // ======== Poison instructions ========

  // Record instructions with poison so we can ignore them later when checking
  // for incorrectly generated instructions.
#ifndef NDEBUG
  SmallSet<Instruction *, 16> PoisonInstructions;
  collectInstWithPoison(F, PoisonInstructions);
#endif

  // ======== Insert Reloads ========

  LLVM_DEBUG(dbgs() << "Inserting Reloads\n");

  // Insert reloads before spills because inserting reloads loops over uses.
  // Spills (inserted below) also count as a use so if we insert spills
  // before reloads then that would add more uses, but we should not insert
  // a reload before a spill. So we insert reloads first.
  insertReloads();

  // ======== Insert Spills ========
  // Spills are done after reloads so we can try to insert spills after
  // last-uses (reloads) when eviction is enabled.

  LLVM_DEBUG(dbgs() << "Inserting Spills\n");

  insertSpills(Shape, DT);

  // ======== Complete Accesses To the Frame Structs ========

  LLVM_DEBUG(dbgs() << "Building Phi Node Networks\n");

  // With all spills and reloads in-place now we can generate the phi network
  // that carries the values between defs and uses.
  buildPhiNetwork();

  LLVM_DEBUG(dbgs() << "Removing unused reloads\n");

  // A value may cross multiple suspends but not be used between the suspends.
  // Now that the phi node networks have been built we can remove reloads that
  // did not end up having any uses.
  removeUnusedReloads();

  LLVM_DEBUG(dbgs() << "Creating GEPs\n");

  // Build GEPs to complete the access to the frame structs. Replace poisoned
  // frame address ptrs with computed values. Also replace allocas with frame
  // address ptrs.
  createFrameGEPs(DeadInstructions);

  LLVM_DEBUG(dbgs() << "Final Frame Size Bytes: " << MaxFrameSize << "\n");
  LLVM_DEBUG(dbgs() << "Final Frame Align Bytes: " << MaxFrameAlign.value() << "\n");

  LLVM_DEBUG(reportContStateInfo());

  LLVM_DEBUG({
    dbgs() << "-- FrameStructs --\n";
    unsigned Idx = 0;
    for (auto &I : FrameStructs) {
      auto *Suspend = I.first;
      auto &Struct = I.second;
      dbgs() << "Suspend " << Idx++ << "\n";
      dbgs() << "\tSuspend: ";
      Suspend->dump();
      Struct.dump(FrameTable);
    }
  });

  LLVM_DEBUG({
    dbgs() << "-- FrameTable --\n";
    unsigned Idx = 0;
    for (auto &Row : FrameTable) {
      dbgs() << "Row " << Idx++ << "\n";
      Row.dump();
    }
  });

  // ======== Poison instructions ========
#ifndef NDEBUG
  // Verify no new poisons are left in the IR
  if (hasNewPoisonOperand(F, PoisonInstructions)) {
    llvm_unreachable("Error: Found poison");
  }
#endif

  // Remove dead instrs
  for (auto *I : DeadInstructions)
    I->eraseFromParent();

  // Info is printed if non-debug mode for stats collection & reporting.
  reportGepsSpillsAndReloads(F, NonFrameGeps, NonSpillStores, NonReloadLoads);

  DEBUG_DUMP_CFG(F, "post-frame-build-cfg");
  LLVM_DEBUG(dbgs() << "-- After buildCoroutineFrame, Before splitCoroutine --\n"; F.dump());
}

} // namespace

ContStateBuilder::ContStateBuilder(Function &F, coro::Shape &S, std::function<bool(Instruction &I)> IsMaterializable)
    : coro::AnyRetconABI(F, S, IsMaterializable) {
}

// Allocate the coroutine frame and do spill/reload as needed.
void ContStateBuilder::buildCoroutineFrame(bool OptimizeFrame) {
#ifndef NDEBUG
  if (UseLLVMContStateBuilder) {
    AnyRetconABI::buildCoroutineFrame(OptimizeFrame);
    return;
  }
#endif

  ContStateBuilderImpl Impl(F, Shape, IsMaterializable);

  Impl.buildCoroutineFrame();
}

#undef DEBUG_TYPE
