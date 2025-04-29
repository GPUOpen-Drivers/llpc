/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/StackLifetime.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Support/OptimizedStructLayout.h"
#include "llvm/Transforms/Coroutines/SpillUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#define DEBUG_TYPE "cont-state-builder"
#define DEBUG_DUMP_CFG(FUNC, MSG)                                                                                      \
  DEBUG_WITH_TYPE("cont-state-cfg-dump", irserializationutils::writeCFGToDotFile(FUNC, MSG))

using namespace llvm;
using namespace llvmraytracing;

namespace {
cl::opt<bool> ReportContStateAccessCounts(
    "report-cont-state-access-counts",
    cl::desc("Report on the number of spills (stores) and reloads (loads) from the cont state."), cl::init(false),
    cl::Hidden);

#ifndef NDEBUG
// When debugging a potential issue with the cont-state-builder try setting
// this option to verify the issue resides within the builder.
cl::opt<bool> UseLLVMContStateBuilder("use-llvm-cont-state-builder",
                                      cl::desc("Use LLVM's built-in continuation state builder."), cl::init(false),
                                      cl::Hidden);
#endif

struct Gap {
  uint64_t Offset, Size;

  Gap(uint64_t Offset, uint64_t Size) : Offset(Offset), Size(Size) {}

  // Only compare Offsets for sorting.
  bool operator<(const Gap &Other) const { return Offset < Other.Offset; }

  uint64_t getEndOffset() const { return Offset + Size; }
};

// Representation of a row in the frame-table.
struct CoroFrameRow {
  CoroFrameRow(const DataLayout &DL, Value *D) : Def(D), IsAlloca(isa<AllocaInst>(D)) {
    // Determine alignment of Def
    if (IsAlloca) {
      auto *AI = cast<AllocaInst>(Def);
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

  // The original definition of the instr, arg or alloca
  Value *Def = nullptr;

  // True if the Def is an AllocaInst.
  bool IsAlloca = false;

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
  // Block is in set if value is spilled there.
  SmallSet<BasicBlock *, 2> SpilledOnBB;
  // True if value is forced to be spilled before each suspend, if 'spill-on=def'.
  bool ForceSpillOnSuspend = false;

  // Block is in set if value is reloaded there.
  SmallSet<BasicBlock *, 2> ReloadedOnBB;
  // True if value is forced to be reloaded on resume, even if 'reload-on=use'.
  bool ForceReloadOnResume = false;

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

  // Compares the row's range with the test offset and size (test range) and
  // returns the signed distance value of:
  //  0 -> Row's range overlaps with the test range,
  //  positive -> Row's range follows the test range,
  //  negative -> Row's range precedes the test range.
  int64_t compareRanges(Gap Test) const;

  // Return the Range the Row occupies in the frame as a Gap.
  Gap getRange() const { return {Offset, Size}; }
};

using CoroFrameTableTy = std::vector<CoroFrameRow>;
using CoroFrameGapsTy = SmallVectorImpl<Gap>;

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

// Data structure that maps from Suspend instructions to the FrameStruct that
// holds state related to the suspend.
using FrameStructMapTy = SmallMapVector<AnyCoroSuspendInst *, CoroFrameStruct, 2>;

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

  // Value to FrameTable Row (index) map -- used to ensure a value always has
  // the same location in the frame.
  using DefRowMapTy = SmallMapVector<Value *, unsigned, 8>;
  DefRowMapTy AllFrameValues;

  // Map of the optimized struct and fields for each suspend's frame.
  FrameStructMapTy FrameStructs;

  // Used to allocate the frame with the size needed to handle the largest
  // computed struct layout and determine if the inline storage is sufficient
  // to hold the frame.
  // Max Frame -- Largest frame required by the suspends.
  // Max Alignment -- Largest individual field's alignment.
  uint64_t MaxFrameSize = 0;
  Align MaxFrameAlign;

  // Helper for building the FrameTable, Count is incremented if a new value is
  // inserted. Returns true if the Def is added, false if it already existed in
  // the FrameTable. Methods works by checking if the Def already exists in the
  // AllFrameValues map. If it does not exist a new Row is created for the Def.
  // In all cases a mapping is added to CurrentFrameValues from the Def to the
  // Row index.
  bool tryInsertFrameTableRow(DefRowMapTy &CurrentFrameValues, Value *Def);

  // Go through candidate list and add values that are needed for the suspend
  // to the frame. Note: the location in the frame is not yet finalized.
  void addValuesToFrameTable(AnyCoroSuspendInst *Suspend, const coro::SpillInfo &CandidateSpills,
                             const SmallVector<coro::AllocaInfo, 8> &CandidateAllocas);

  // Make the rows reside in the given suspend's frame
  void makeRowsResideInSuspendFrame(DefRowMapTy &ValueRows, AnyCoroSuspendInst *Suspend);

  // Determine location of gaps in the current frame struct layout.
  void initFrameStructLayout(CoroFrameGapsTy &Gaps, AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct);

  // Returns true if the range formed by CandidateOffset and Row.Size conflicts
  // with Rows that already have a place in the frame.
  bool hasConflict(uint64_t CandidateOffset, const CoroFrameRow &Row, const StackLifetime &StackLifetimeAnalyzer,
                   uint64_t &NextGapStep);

  // Return true if the (CandidateOffset, Row.Size) candidate range for Row
  // overlaps other Rows in the frame structs that value occupies.
  bool hasFrameStructConflict(uint64_t CandidateOffset, const CoroFrameRow &Row, uint64_t &NextGapStep);

  // Return true if lifetime of Row's alloca conflicts with the lifetime of any
  // other Row that overlaps with the range (CandidateOffset, Row.Size).
  bool hasStackLifetimeConflict(uint64_t CandidateOffset, const CoroFrameRow &Row,
                                const StackLifetime &StackLifetimeAnalyzer, uint64_t &NextGapStep);

  // Determine if the value given by NewField fits into the Gap. Conflicts and
  // interference are checked with other values that occupy the gap.
  bool tryFitInGap(Gap Gap, CoroFrameRow &Row, const StackLifetime &StackLifetimeAnalyzer);

  // Iterates over the gaps and tries to find a place to fit the NewField. The
  // lambda will set the NewField.Offset and return true if it fits in the gap.
  // The method will then create the necessary gaps around the NewField.
  bool findGapForRow(CoroFrameRow &Row, CoroFrameGapsTy &Gaps, const StackLifetime &StackLifetimeAnalyzer);

  // Layout fields according to program order.
  void computeFrameStructLayoutGreedy(AnyCoroSuspendInst *Suspend, CoroFrameGapsTy &Gaps,
                                      const StackLifetime &StackLifetimeAnalyzer);

  // Finalize the struct layout by sorting for spilling and reload, and
  // determining the max frame size and alignments.
  void finalizeFrameStructLayout(CoroFrameStruct &Struct);

  // Analyze and report on the type of values that are unused in the current frame.
  void unusedValueAnalysis(const CoroFrameTableTy &FrameTable, const coro::SpillInfo &CandidateSpills,
                           const SmallVectorImpl<coro::AllocaInfo> &CandidateAllocas) const;

  // Analyze and report on the fragmentation of the current frame.
  void fragmentationAnalysis(const CoroFrameTableTy &FrameTable,
                             SmallVectorImpl<OptimizedStructLayoutField> &StructFields,
                             const coro::SpillInfo &CandidateSpills,
                             const SmallVectorImpl<coro::AllocaInfo> &CandidateAllocas) const;

  // When eviction is enabled then reuse of the frame memory can cause
  // interference between the values stored there. This identifies the
  // interfering rows/values and modifies the spill and reload strategies
  // to avoid corrupting the frame values -- to prevent a spill before an
  // interfering value's last reload.
  void computeInterference(ArrayRef<OptimizedStructLayoutField> StructFields);

  // Create the frame type, its size is the maximum of the frame sizes
  // required at each suspend.
  StructType *createFrameTy() const;

  // In the following spill and reload methods the new insts are added to the
  // insts FrameRow::Reloads and FrameRow::Spills so we can build its phi node
  // network later.

  // Insert spills and reloads
  void insertSpills(coro::Shape &Shape, DominatorTree &DT, LoopInfo &LI);
  void insertReloads(DominatorTree &DT);

  // With all spills and reloads in-place now we can generate the phi network
  // that carries the values between defs and uses.
  void buildPhiNetwork();

  // Replace poisoned frame-address values with computed values.
  void createFrameGEPs(SmallVector<Instruction *, 4> &DeadInstructions);

  // Remove unused reloads
  void removeUnusedReloads();

  // Report stats collected by FrameTable and FrameStruct data structures
  void reportContStateInfo() const;
};

enum ContStateBuilderMode {
  Baseline = 0,  // unoptimized baseline
  ContOpt = 1,   // continuations optimized baseline
  SimVgprEx = 2, // simulates vgpr exchange
};

cl::opt<ContStateBuilderMode>
    ContStateBuilderMode("cont-state-builder-mode",
                         cl::desc("Set the strategy for frame layout, spilling and reloading"),
                         cl::init(ContStateBuilderMode::ContOpt),
                         values(clEnumValN(ContStateBuilderMode::Baseline, "baseline", "Similar to LLVM's CoroFrame"),
                                clEnumValN(ContStateBuilderMode::ContOpt, "contopt", "Optimized for continuations"),
                                clEnumValN(ContStateBuilderMode::SimVgprEx, "simvgprex", "Simulate vgpr exchange")));

bool isEvictNone() {
  return ContStateBuilderMode == Baseline || ContStateBuilderMode == ContOpt;
}

bool isEvictUnused() {
  return ContStateBuilderMode == SimVgprEx;
}

[[maybe_unused]] bool isSpillOnDef() {
  return ContStateBuilderMode == Baseline || ContStateBuilderMode == ContOpt;
}

bool isSpillOnSuspend() {
  return ContStateBuilderMode == SimVgprEx;
}

[[maybe_unused]] bool isReloadOnUse() {
  return ContStateBuilderMode == Baseline || ContStateBuilderMode == ContOpt;
}

bool isReloadOnResume() {
  return ContStateBuilderMode == SimVgprEx;
}

/// Return true if Def is an Arg with the ByVal attribute.
[[maybe_unused]] bool isArgByVal(Value *Def) {
  if (auto *Arg = dyn_cast<Argument>(Def))
    return Arg->hasByValAttr();
  return false;
}

void CoroFrameRow::dump() const {
  if (Def) {
    dbgs() << "\tDef: ";
    LLVM_DEBUG(Def->dump());
    if (isa<Instruction>(Def))
      dbgs() << "\tDefBB: %" << compilerutils::bb::getLabel(cast<Instruction>(Def)->getParent()) << "\n";
    else if (isa<Argument>(Def))
      dbgs() << "\tDefBB: %" << compilerutils::bb::getLabel(&cast<Argument>(Def)->getParent()->getEntryBlock()) << "\n";
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
  if (!IsAlloca) {
    dbgs() << "\tSpilledOnDef: " << (SpilledOnDef ? "true" : "false") << "\n";
    dbgs() << "\tReloadedOnBB: " << compilerutils::bb::getNamesForBasicBlocks(ReloadedOnBB) << "\n";
    dbgs() << "\tForceSpillOnSuspend: " << (ForceSpillOnSuspend ? "true" : "false") << "\n";
    dbgs() << "\tForceReloadOnResume: " << (ForceReloadOnResume ? "true" : "false") << "\n";
    dbgs() << "\tSpills: " << Spills.size() << "\n";
    dbgs() << "\tReloads: " << Reloads.size() << "\n";
  }
}

int64_t CoroFrameRow::compareRanges(Gap Test) const {
  assert(Offset != OptimizedStructLayoutField::FlexibleOffset);

  // Stop if the start addr of the Row exceeds the test range's end addr.
  // Row's range comes after the test range.
  if (Offset >= Test.Offset + Test.Size) {
    int64_t Diff = Offset - (int64_t)(Test.Offset + Test.Size - 1); // Positive value
    assert(Diff > 0);
    return Diff;
  }

  // Stop if the test range's start addr exceeds the end addr of the Row.
  // Row's range comes before the test ranges.
  if (Offset + Size <= Test.Offset) {
    int64_t Diff = (Offset + Size - 1) - (int64_t)Test.Offset; // Negative value
    assert(Diff < 0);
    return Diff;
  }

  // Row's range overlaps with test range, 3 cases:
  //  Row starts at the same addr as test range
  //  Row starts at an earlier addr but ends after test ranges' start addr
  //  Row starts at a later addr but before the end of test ranges' end addr
  assert((Offset == Test.Offset) || (Offset < Test.Offset && Offset + Size > Test.Offset) ||
         (Offset > Test.Offset && Offset < Test.Offset + Test.Size));

  return 0;
}

void CoroFrameStruct::dumpField(const OptimizedStructLayoutField &F, const CoroFrameTableTy &FrameTable) const {
  auto Idx = reinterpret_cast<size_t>(F.Id);
  const CoroFrameRow *Row = &FrameTable[Idx];
  dbgs() << " Frame Table Row " << std::to_string(Idx);
  if (Row->IsAlloca)
    dbgs() << " -- Alloca for %" << compilerutils::bb::getLabel(Row->Def);
  else if (isa<Argument>(Row->Def))
    dbgs() << " -- Spill of Argument %" << compilerutils::bb::getLabel(Row->Def);
  else
    dbgs() << " -- Spill of Inst %" << compilerutils::bb::getLabel(Row->Def);

  // Determine if value is a spill or alloca
  if (Row->IsAlloca) {
    auto *DefAlloca = cast<AllocaInst>(Row->Def);
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
  std::string SuspendBBName = SuspendBB ? compilerutils::bb::getLabel(SuspendBB) : "nullptr";
  dbgs() << "\tSuspendBB: %" << SuspendBBName << "\n";
  std::string ResumeBBName = ResumeBB ? compilerutils::bb::getLabel(ResumeBB) : "nullptr";
  dbgs() << "\tResumeBB: %" << ResumeBBName << "\n";
}

bool ContStateBuilderImpl::tryInsertFrameTableRow(DefRowMapTy &CurrentFrameValues, Value *Def) {
  auto Idx = FrameTable.size();

  assert(Def);
  auto [Itr, Inserted] = AllFrameValues.try_emplace(Def, Idx);
  auto &[ExistingRowVal, ExistingRowIdx] = *Itr;

  if (Inserted) {
    // Add new value
    FrameTable.emplace_back(CoroFrameRow(DL, Def));
  } else {
    // Reuse existing row
    assert(ExistingRowVal == Def);
    Idx = ExistingRowIdx;
  }

  // No need to keep track of the current frame values if we are not evicting
  // unused values.
  if (isEvictUnused())
    CurrentFrameValues[Def] = Idx;

  return Inserted;
}

void ContStateBuilderImpl::addValuesToFrameTable(AnyCoroSuspendInst *Suspend, const coro::SpillInfo &CandidateSpills,
                                                 const SmallVector<coro::AllocaInfo, 8> &CandidateAllocas) {
  [[maybe_unused]] unsigned NewArgBytes = 0;
  [[maybe_unused]] unsigned NewInstBytes = 0;
  [[maybe_unused]] unsigned NewAllocaBytes = 0;

  DefRowMapTy CurrentFrameValues;

  // Add candidate spills. For each suspend that the value crosses it will be
  // added to its frame. The def will be spilled to the frame and a load from
  // the frame will occur before uses where the def-use crosses the suspend.
  for (auto &[Def, Aliases] : CandidateSpills) {
    if (tryInsertFrameTableRow(CurrentFrameValues, Def)) {
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

    if (tryInsertFrameTableRow(CurrentFrameValues, AI.Alloca)) {
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

  // Adding FrameValues rows to the given suspend's frame. Adding only the
  // CurrentFrameValues rows will cause gaps to appear where values are no
  // longer needed. Adding AllFrameValues rows will prevent values from begin
  // overwritten if they are no longer needed.
  if (isEvictUnused())
    makeRowsResideInSuspendFrame(CurrentFrameValues, Suspend);
  else {
    assert(isEvictNone());
    makeRowsResideInSuspendFrame(AllFrameValues, Suspend);
  }
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
bool isSuspendCrossingValue(const CoroFrameRow &Row, const coro::SpillInfo &CandidateSpills,
                            const SmallVectorImpl<coro::AllocaInfo> &CandidateAllocas) {
  if (Row.IsAlloca) {
    auto *DefAlloca = cast<AllocaInst>(Row.Def);
    auto II = std::find_if(CandidateAllocas.begin(), CandidateAllocas.end(),
                           [DefAlloca](const coro::AllocaInfo &AI) { return AI.Alloca == DefAlloca; });
    return II != CandidateAllocas.end();
  }

  return CandidateSpills.contains(Row.Def);
}

static void findGaps(CoroFrameGapsTy &Gaps, const SmallVectorImpl<OptimizedStructLayoutField> &Fields) {
  if (Fields.empty())
    return;

  uint64_t EndOffset = 0;

  // Scan fields, that must be in order, and identify the gaps between them.
  for (auto &Field : Fields) {
    uint64_t NextStartOffset = Field.Offset;
    assert(EndOffset <= NextStartOffset && "Fields must be sorted");

    if (EndOffset < NextStartOffset)
      Gaps.emplace_back(EndOffset, NextStartOffset - EndOffset);

    EndOffset = NextStartOffset + Field.Size;
  }
}

void ContStateBuilderImpl::initFrameStructLayout(CoroFrameGapsTy &Gaps, AnyCoroSuspendInst *Suspend,
                                                 CoroFrameStruct &Struct) {
  // The Greedy layout optimization adds new fields to each FrameStruct that
  // the value resides in, so there is no need to add them again here. While
  // doing so each FrameStructs size and alignment are updated.

  // Sort the fixed offset fields to identify gaps between existing values.
  llvm::sort(
      Struct.Fields.begin(), Struct.Fields.end(),
      [&](const OptimizedStructLayoutField &A, const OptimizedStructLayoutField &B) { return A.Offset < B.Offset; });

  // After sorting last element in Fields is the last in memory.
  assert(Struct.Fields.empty() || Struct.Size == Struct.Fields.back().getEndOffset());

  // Determine gaps, if we don't evict values then just add new ones at the
  // end, don't try to fill gaps.
  if (isEvictUnused())
    findGaps(Gaps, Struct.Fields); // Note, gaps are sorted by their offsets
}

static void splitGapAroundNewField(CoroFrameGapsTy::iterator Itr, Gap NewField, CoroFrameGapsTy &Gaps) {
  // Split gap around new field position as needed
  assert(NewField.Offset >= Itr->Offset && (Itr->Offset + Itr->Size) >= NewField.getEndOffset());

  uint64_t BeforeGapSize = NewField.Offset - Itr->Offset;
  uint64_t AfterGapSize = (Itr->Offset + Itr->Size) - NewField.getEndOffset();

  if (BeforeGapSize == 0 && AfterGapSize == 0) {
    // Remove the old gap
    Gaps.erase(Itr);
  } else if ((BeforeGapSize > 0) ^ (AfterGapSize > 0)) {
    // There is a gap before the start of the field
    if (BeforeGapSize > 0)
      Itr->Size = BeforeGapSize;
    else {
      // There is a gap after the end of the field
      Itr->Offset = NewField.getEndOffset();
      Itr->Size = AfterGapSize;
    }
  } else {
    // There is a gap before the start of the field
    Itr->Size = BeforeGapSize;

    // There is a gap after the end of the field
    Gaps.emplace_back(NewField.getEndOffset(), AfterGapSize);

    // Keep gaps sorted by their offsets so we fill them in order
    llvm::sort(Gaps.begin(), Gaps.end());
  }
}

bool ContStateBuilderImpl::hasConflict(uint64_t CandidateOffset, const CoroFrameRow &Row,
                                       const StackLifetime &StackLifetimeAnalyzer, uint64_t &NextGapStep) {
  if (hasFrameStructConflict(CandidateOffset, Row, NextGapStep))
    return true;

  // Row is a non-alloca, so it has no stacklifetime
  if (!Row.IsAlloca)
    return false;

  return hasStackLifetimeConflict(CandidateOffset, Row, StackLifetimeAnalyzer, NextGapStep);
}

// Check if the range formed by the CandidateOffset and NewField.Size
// conflicts with other values in any other frame structs. Note, that if
// a conflict is found the NextGapStep is updated such that the conflicting
// value will not be re-tested when tryFitInGap steps further into the gap.
bool ContStateBuilderImpl::hasFrameStructConflict(uint64_t CandidateOffset, const CoroFrameRow &Row,
                                                  uint64_t &NextGapStep) {
  // TODO: to optimize this add an std::bitset to the FrameStruct. When a
  // value is added to a FrameStruct, the bitsets of all FrameStructs
  // that include the value (via ResidesInSuspendFrame) are updated. Now
  // we can optimize the following by scanniinig over FrameStructs instead of
  // all FrameTable rows. For each struct checking for a conflict is then a
  // simple matter of checking if Row.Size bits starting at the candidate
  // offset are occupied (set) in the struct's bitset.

  // Check already laid out rows that reside a frame that the current row
  // also resides in for a conflict.
  for (auto &OtherRow : FrameTable) {
    // Skip the current row, i.e. find 2 unique rows.
    if (&Row == &OtherRow)
      continue;

    // Skip rows that have not yet been laid out.
    if (OtherRow.Offset == OptimizedStructLayoutField::FlexibleOffset)
      continue;

    // OtherRow has an offset, so we need to check if it occupies a frame
    // with current Row. If both are in the same frame, then we need to
    // check if the candidate offsets overlaps with OtherRow.

    for (auto *Suspend : OtherRow.ResidesInSuspendFrame) {
      // Skip this suspend if the current row is not also a member of its
      // frame. This finds the frames that include both Row and OtherRow.
      if (!Row.ResidesInSuspendFrame.contains(Suspend))
        continue;

      // Reject candidate offset if it conflicts with OtherRow.
      if (OtherRow.compareRanges({CandidateOffset, Row.Size}) == 0) {
        // If there is a conflict, then we step to the next untested gap.
        NextGapStep = std::max(OtherRow.Size, NextGapStep);

        return true;
      }

      // We have verified that OtherRow does not conflict.
      break;
    }
  }

  // The candidate offsets does not conflict with fields in any other
  // structs.
  return false;
}

// Check if the range formed by the CandidateOffset and Row.Size
// interferes with other values. For non-alloca this always returns true
// because we can place loads and stores to mitigate potential interference.
// For alloca this will use the stack lifetime analyzer to determine if it
// interferes with any alloca that has already been laid out. Note, that if
// interference is found the NextGapStep is updated such that the interfering
// alloca will not be re-tested when tryFitInGap steps further into the gap.
bool ContStateBuilderImpl::hasStackLifetimeConflict(uint64_t CandidateOffset, const CoroFrameRow &Row,
                                                    const StackLifetime &StackLifetimeAnalyzer, uint64_t &NextGapStep) {
  assert(Row.IsAlloca);

  auto *Alloca = cast<AllocaInst>(Row.Def);

  // New field is an alloca, so we must check interference with other alloca.
  for (auto &OtherRow : FrameTable) {
    // Skip the current row, i.e. find 2 unique rows.
    if (&Row == &OtherRow)
      continue;

    // Only consider rows that with an Alloca def
    if (!OtherRow.IsAlloca)
      continue; // OtherRow is not an alloca

    // Don't consider rows without offsets
    if (OtherRow.Offset == OptimizedStructLayoutField::FlexibleOffset)
      continue; // OtherRow has not been placed in the frame yet.

    // Test if gap overlaps with OtherRow, if not then the gap does not
    // potentially interfere with the OtherRow.
    if (OtherRow.compareRanges({CandidateOffset, Row.Size}) != 0)
      continue; // OtherRow does not interfere

    // Now we have found an alloca that shares space in the frame with the Row.
    // So we need to check if there is actual interference. The lifetime
    // analyzer is used to check for actual interference.
    auto *OtherAlloca = cast<AllocaInst>(OtherRow.Def);
    if (StackLifetimeAnalyzer.getLiveRange(Alloca).overlaps(StackLifetimeAnalyzer.getLiveRange(OtherAlloca))) {
      // If there is a interference, then we step to the next untested gap.
      NextGapStep = std::max(OtherRow.Size, NextGapStep);

      return true;
    }

    // We have verified that there the CandidateOffset does not interfere.
    break;
  }

  // The candidate offsets does not interfere with any other Rows.
  return false;
}

// This method searches the given Gap for a non-conflicting and non-interfering
// offset for the Row. If a valid Offset is found the Row.Offset is updated and
// the method returns true. Otherwise it returns false.
bool ContStateBuilderImpl::tryFitInGap(Gap Gap, CoroFrameRow &Row, const StackLifetime &StackLifetimeAnalyzer) {
  assert(Row.Offset == OptimizedStructLayoutField::FlexibleOffset);

  // Loop while there is unchecked space in the Gap.
  while (Gap.Size > 0) {
    auto AdjustedFieldOffset = alignTo(Gap.Offset, Row.Alignment);
    assert(AdjustedFieldOffset >= Gap.Offset);

    // Check that there is enough room for the alloca in the gap after
    // considering the alloca's alignment.
    auto RequiredFieldSize = Row.Size + (AdjustedFieldOffset - Gap.Offset);
    assert(RequiredFieldSize >= Row.Size);
    if (RequiredFieldSize > Gap.Size)
      return false;

    auto NextGapStep = RequiredFieldSize;

    // Check for conflicts with other Rows, if none then we found a place to
    // insert the new field.
    if (!hasConflict(AdjustedFieldOffset, Row, StackLifetimeAnalyzer, NextGapStep)) {
      Row.Offset = AdjustedFieldOffset;
      return true;
    }

    // Shrink the gap by skipping over the space with interference.
    Gap.Offset += NextGapStep;
    Gap.Size -= NextGapStep;
  }

  return false;
}

// Iterates over the gaps and tries to find a place to fit the Row.
// If an Offset for Row is found the Gap the Offset resides in will be split.
bool ContStateBuilderImpl::findGapForRow(CoroFrameRow &Row, CoroFrameGapsTy &Gaps,
                                         const StackLifetime &StackLifetimeAnalyzer) {
  // If we can find a gap big enough fit the new field in there
  for (auto Itr = Gaps.begin(); Itr != Gaps.end(); ++Itr) {
    if (!tryFitInGap(*Itr, Row, StackLifetimeAnalyzer))
      continue;

    splitGapAroundNewField(Itr, Row.getRange(), Gaps);

    return true;
  }

  return false;
}

// Do a greedy layout of the frame of the Rows that cross the given Suspend,
// skipping those that have already been laid out.
void ContStateBuilderImpl::computeFrameStructLayoutGreedy(AnyCoroSuspendInst *Suspend, CoroFrameGapsTy &Gaps,
                                                          const StackLifetime &StackLifetimeAnalyzer) {
  // Add flexible fields into the gaps
  for (auto [Idx, Row] : llvm::enumerate(FrameTable)) {
    // Skip if Row has an Offset or does not occupy this suspend's frame.
    if (Row.Offset != OptimizedStructLayoutField::FlexibleOffset || !Row.ResidesInSuspendFrame.contains(Suspend))
      continue;

    if (!findGapForRow(Row, Gaps, StackLifetimeAnalyzer)) {
      // If the field could not be added into a gap, then we just add it to
      // the end. But we need to choose an offset that will not conflict with
      // other frames this value may be in. So iterate over the FrameStructs
      // this value is in to find a safe offset.

      uint64_t MaxResidingStructSize = 0;

      // Take a maximum of all structs that contain this value
      for (auto &[OtherSuspend, OtherStruct] : FrameStructs) {
        // Determine if this row resides in the other suspend's frame, skip
        // the check if the OtherSuspend is equal to Suspend.
        if (Suspend != OtherSuspend && !Row.ResidesInSuspendFrame.contains(OtherSuspend))
          continue;

        // Update struct size
        MaxResidingStructSize = std::max(MaxResidingStructSize, OtherStruct.Size);
      }

      // Row's offset is located after the largest frame that contains the
      // value, so we can be sure it won't conflict.
      Row.Offset = alignTo(MaxResidingStructSize, Row.Alignment);
    }

    // Value is in this frame, create a 'field' for it.
    void *VoidIdx = reinterpret_cast<void *>(Idx);
    OptimizedStructLayoutField NewField = {VoidIdx, Row.Size, Row.Alignment, Row.Offset};

    // Offset is assigned and aligned correctly
    assert(NewField.Offset != OptimizedStructLayoutField::FlexibleOffset);
    assert(NewField.Offset == alignTo(NewField.Offset, NewField.Alignment));

    // Update the Row's Offset in the FrameTable
    Row.Offset = NewField.Offset;

    // Now add the field to the structs it resides in and update each struct's
    // size and alignment.
    for (auto &[OtherSuspend, OtherStruct] : FrameStructs) {
      // Determine if this row resides in the other suspend's frame, skip the
      // check if the OtherSuspend is equal to Suspend.
      if (Suspend != OtherSuspend && !Row.ResidesInSuspendFrame.contains(OtherSuspend))
        continue;

      // Update struct size and alignment
      OtherStruct.Size = std::max(OtherStruct.Size, NewField.getEndOffset());
      OtherStruct.Alignment = std::max(OtherStruct.Alignment, NewField.Alignment);

      // Add the new field
      OtherStruct.Fields.emplace_back(NewField);
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

void ContStateBuilderImpl::computeInterference(ArrayRef<OptimizedStructLayoutField> StructFields) {
  assert(isEvictUnused());

  // If spill-on=suspend then there will be no interference with spills or
  // reloads.
  if (isSpillOnSuspend())
    return;

  // isReloadOnResume() == true does not prevent interference on its own as
  // the spills themselves may interfere. For example, consider 2 sequential
  // defs followed by 2 conditional suspends (e.g. a diamond cfg), the first
  // def is used after the first suspend, and the second def is used after
  // the second suspend def. Eviction may allow second def to take the space
  // of the first, but because the defs are sequential if we enter the first
  // suspend the first defs value will be overwritten, assuming spill-on=def.

  // Scan through the FrameTable checking the offsets against the allocated
  // fields in the current frame. If there is an overlap then the reloading
  // and spilling will need to be modified to account for the interferences.
  // This is because a value's spill must occur after the last reload of any
  // values it interferes with in the frame memory. Currently, we just
  // require those fields to spill-on suspend, but the change could be more
  // complex.
  for (CoroFrameRow &PreValRow : FrameTable) {
    // Value has not yet been laid out.
    if (PreValRow.Offset == OptimizedStructLayoutField::FlexibleOffset)
      continue;

    // Loop over the CurrentValues in the frame
    for (auto &Field : StructFields) {
      auto Idx = reinterpret_cast<size_t>(Field.Id);
      CoroFrameRow &CurValRow = FrameTable[Idx];
      // Ignore if CurValRow and PreValRow are the same row
      if (&PreValRow == &CurValRow)
        continue;

      // Interference of allocas with allocas is handled earlier.
      if (PreValRow.IsAlloca && CurValRow.IsAlloca)
        continue;

      auto Diff = PreValRow.compareRanges({CurValRow.Offset, CurValRow.Size});

      // PreValRow's range comes after CurValRow's range, so go to next
      // CurValRow.
      if (Diff > 0)
        continue;

      // PreValRow's range comes before CurValRow's range, since
      // StructFields is sorted we can conclude there is no interference.
      if (Diff < 0)
        break;

      // There is potential interference, so there is a risk of corruption
      // i.e. overwriting the memory before reading its previous value.

      // TODO: Although we know here that PreValRow and CurValRow use the
      // same space in the frame we don't actually know if the reloads of
      // PreValRow interfere with the spills of CurValRow. It is 'safe' to
      // assume they do, but we could improve this by checking if there is
      // actual interference. This may require pre-computing the spill and
      // reload locations.

      if (CurValRow.IsAlloca) {
        // PreValRow is a non-alloca, so force reload-on=resume to ensure its
        // value is read before the alloca.
        PreValRow.ForceReloadOnResume = true;
        continue;
      }

      // Both values are non-alloca.

      // Force CurValRow to spill-on=suspend. Note that this only applies to
      // the current value, but it will cause that value to spill-on=suspend
      // for all suspend points, not just those with potential interference.
      CurValRow.ForceSpillOnSuspend = true;

      // For now it is necessary to force reload-on=resume when also forcing
      // spill-on=suspend. This is because, in the case of a conditional
      // suspend followed by another suspend it is currently necessary to
      // reload the value after the first suspend so it can be stored again
      // before the second. This is not ideal. TODO: Remove this once cloning
      // is modified such that the extra spill in the resume following the
      // conditional suspend can be removed.
      CurValRow.ForceReloadOnResume = true;
    }
  }
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

// A loop preheader is a single BB that precedes a single loop entry point.
// We may need to insert spills into the preheader if the def is from outside
// the loop. This method is used to create loop preheaders when they do not
// exist according to loop analysis.
static void createLoopPreHeadersIfMissing(DominatorTree &DT, LoopInfo &LI) {
  for (Loop *L : LI) {
    // Determine if the loop preheader exists
    auto *BB = L->getLoopPreheader();

    // The preheader may be null if the loop has multiple predecessors,
    // or if it is not legal to hoist instrs into the single predecessor.
    // If this occurs we create a landing block.
    if (BB)
      continue;

    // Insert may fail due to a failure of SplitBlockPredecessors. Although
    // this is not expected to happen it is asserted here for sanity. We handle
    // this case when inserting reloads.
    [[maybe_unused]] auto *NewBB = llvm::InsertPreheaderForLoop(L, &DT, &LI, nullptr, /*PreserveLCSSA*/ false);
    assert(NewBB);
  }
}

// This method searches for spills that are dominated by other spills and thus
// can be safely removed. For example:
//   spill A.1 /* loop preheader */
//   for() { suspend }
//   spill A.2 /* candidate for removal */
//   suspend
// In the above example, A.1 is the dominator spill and A.2 is the dominated spill.
void removeDominatedSpills(CoroFrameRow &Row, DominatorTree &DT) {
  for (auto Itr = Row.Spills.begin(); Itr != Row.Spills.end();) {
    // S is a candidate spill to remove.

    auto *S = *Itr;
    bool IsDominated = false;

    // Check all other spills if they are dominated by S.
    for (auto *OtherS : Row.Spills) {
      // OtherS is a potential dominator.

      if (S == OtherS)
        continue;

      // Check if S is dominated by OtherS, i.e. OtherS will execute before S.
      if (DT.dominates(OtherS, S)) {
        IsDominated = true;
        break;
      }
    }

    // Erase the spill if it is dominated.
    if (IsDominated) {
      Itr = Row.Spills.erase(Itr);
      Row.SpilledOnBB.erase(S->getParent());
      S->eraseFromParent();
      continue;
    }

    // Advance the iterator if S is not erased.
    Itr++;
  }
}

// Return true if the BB is dominated by any of the Insts.
template <typename A> bool dominates(const A &Insts, BasicBlock *BB, DominatorTree &DT) {
  for (auto *I : Insts)
    if (DT.dominates(I, BB))
      return true;

  return false;
}

// Take InsertBB, a block that is potentially in a loop, return a BB that is in
// the same loop nest (at the same level) as RowDef. If the RowDef is outside a
// loop and InsertBB is in a loop the method will return the loop preheader
// that is within the same loop nest as RowDef.
BasicBlock *getLoopPreheaderIfRequired(BasicBlock *InsertBB, coro::Shape &Shape, Value *RowDef, LoopInfo &LI,
                                       DominatorTree &DT) {
  // So to ensure we spill in the right BB we first determine the inner
  // most loop that contains the SuspendBB, if any.
  auto *L = LI.getLoopFor(InsertBB);
  auto *RowDefInst = dyn_cast<Instruction>(RowDef);

  // If the tentative spill point is in a loop that does not contain the
  // definition of the value, move the spill point to the preheader of the
  // outermost loop that does not contain the definition. This avoids redundant
  // spills in each iteration of the loop(s).
  if (L && (!RowDefInst || !L->contains(RowDefInst))) {
    while (auto ParentL = L->getParentLoop()) {
      // We are done if the def is in the parent loop, if the def is a
      // function Argument then we find the outer most loop.
      if (RowDefInst && ParentL->contains(RowDefInst))
        break;

      // Repeat the process with the loop's parent loop until we are
      // spilling in a loop or non-loop BB that contains the def.
      L = ParentL;
    }

    // Insert into the loop preheader.
    InsertBB = L->getLoopPreheader();
  }

  return InsertBB;
}

// Find the earliest point to spill, before the last load in the block, being
// careful not to pass any calls to llvm.coro or continuation intrinsics/funcs.
BasicBlock::iterator findEarliestInsertPt(BasicBlock *InsertBB, Value *RowDef) {
  // Insertion point should precede the terminator.
  auto InsertPt = InsertBB->getTerminator()->getIterator();
  auto *FirstInst = &*InsertBB->getFirstInsertionPt();

  // Iterate from the bottom up.
  for (auto &I : reverse(*InsertBB)) {
    // Don't go past the def, if it is here
    if (&I == RowDef)
      break;

    // Don't go past the last reload, at this point all reloads have
    // a poison address.
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (isa<PoisonValue>(LI->getPointerOperand()))
        break;
    }

    // Don't go past phi
    if (isa<PHINode>(&I))
      break;

    // Don't go past a Call to continuation.* or llvm.coro.*
    if (CallInst *CI = dyn_cast<CallInst>(&I)) {
      // Indirect calls do not have a function body.
      if (auto *CF = CI->getCalledFunction()) {
        auto CFName = CF->getName();
        if (CFName.starts_with("continuation") || CFName.starts_with("llvm.coro"))
          break;
      }
    }

    InsertPt = I.getIterator();

    // Don't leave the BB
    if (&I == FirstInst)
      break;
  }

  return InsertPt;
}

void ContStateBuilderImpl::insertSpills(coro::Shape &Shape, DominatorTree &DT, LoopInfo &LI) {
  LLVMContext &C = F.getContext();
  IRBuilder<> Builder(C);

  // Determine if the spill is needed for this def and set the insertion pt.
  auto SetInsertPtIfRequired = [&](AnyCoroSuspendInst *Suspend, CoroFrameStruct &Struct, CoroFrameRow &Row) {
    BasicBlock::iterator InsertPt;
    BasicBlock *InsertBB = nullptr;

    if (isSpillOnSuspend() || Row.ForceSpillOnSuspend) {
      // Check that there are no crossings between SuspendBB and Row.Def. In
      // general we don't want to spill a value more than once. So we check
      // the ResidesInSuspendFrame and ensure none of these dominate the
      // SuspendBB. If one does then that means there is another SuspendBB
      // that came before this one that has the spill.
      if (dominates(Row.ResidesInSuspendFrame, Struct.SuspendBB, DT)) {
        // Don't spill because the def crosses another suspend.
        return false;
      }

      InsertBB = Struct.SuspendBB;

      // If the value is defined outside of a loop, and used within a loop
      // that also has a suspend, then we prefer not to spill the value at the
      // suspend. Instead we should spill the value in the loop preheader.
      // Note that if the value is modified on the backedge then there will be
      // a phi at the top of the loop that will be the crossing value. TODO:
      // spill the incoming value in the loop preheader and spill the other
      // incoming values on def to try to avoid spilling if the redef is
      // conditional.
      InsertBB = getLoopPreheaderIfRequired(InsertBB, Shape, Row.Def, LI, DT);

      // Note, earlier we assert that LLVM's InsertPreheaderForLoop gives a
      // non-null result. Consequently, InsertBB will not be null here. It is
      // not safe to fall-back to spill-on-def when forcing spill-on-suspend to
      // remedy potential interference.
      assert(InsertBB);

      // Find the earliest point to spill in the InsertBB, do not pass the def.
      InsertPt = findEarliestInsertPt(InsertBB, Row.Def);
    } else if (!Row.SpilledOnDef) {
      assert(isSpillOnDef());

      InsertPt = coro::getSpillInsertionPt(Shape, Row.Def, DT);
      InsertBB = InsertPt->getParent();

      Row.SpilledOnDef = true;
    } else {
      return false;
    }

    // Insert instructions before InsertPt at the end of the InsertBB.
    auto [Itr, Inserted] = Row.SpilledOnBB.insert(InsertBB);
    if (Inserted)
      Builder.SetInsertPoint(InsertPt);
    return Inserted;
  };

  for (auto &[Suspend, Struct] : FrameStructs) {
    // For each value in the frame insert spill, if they do not already exist.
    // Note: the location in the frame will be set when GEPs are built later
    // for now the addresses are poisoned.

    // Visit each field in the struct and create spills as needed. Visit fields
    // in reverse order to cause the spills to occur in-order after creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<size_t>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];

      // Allocas in the frame do not require spilling.
      if (Row.IsAlloca)
        continue;

      // Do not spill here if the value does not cross this suspend. Note
      // this check is needed when eviction is not used. Without eviction
      // the frame will include values that do not cross it and we should
      // not spill the value on suspends the value does not cross. That
      // will lead to excess spilling and incorrect codegen.
      if (isEvictNone() && !isSuspendCrossingValue(Row, Struct.CandidateSpills, Struct.CandidateAllocas))
        continue;

      if (!SetInsertPtIfRequired(Suspend, Struct, Row))
        continue;

      // Generate a frame address of the Def, poison for now
      Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

      // Generate spill for Def
      StoreInst *Spill = Builder.CreateAlignedStore(Row.Def, PoisonFrameAddr, Row.Alignment);

      // Record spill so we can build the phi node network and fix the frame
      // address later.
      assert(Spill);
      Row.Spills.emplace_back(Spill);
    }
  }

  // If a def may reach multiple suspends (without crossing another suspend)
  // then duplicate spills may occur. This happens when we are inserting a
  // spill at a suspend point. A loop preheader may dominate other spills. To
  // avoid duplicate spills we remove the dominated spills now.
  for (CoroFrameRow &Row : FrameTable) {
    removeDominatedSpills(Row, DT);
  }
}

void ContStateBuilderImpl::insertReloads(DominatorTree &DT) {
  LLVMContext &C = F.getContext();
  IRBuilder<> Builder(C);

  // Generate a frame address of the Def, poison for now.
  Value *PoisonFrameAddr = PoisonValue::get(PointerType::get(C, 0));

  // Insert reloads as needed for this use and returns true if IR is changed.
  auto InsertReloadsForUse = [&](CoroFrameStruct &Struct, CoroFrameRow &Row, size_t Idx, User *U) {
    assert(isReloadOnUse());

    auto *UseBB = cast<Instruction>(U)->getParent();

    // Mark the reloaded BB so we don't reload it a second time
    auto [Itr, Inserted] = Row.ReloadedOnBB.insert(UseBB);

    // A reload already exists here, no changes to IR.
    if (!Inserted)
      return false;

    // Reload before the use
    Builder.SetInsertPoint(UseBB, UseBB->getFirstInsertionPt());
    auto *CurrentReload = Builder.CreateAlignedLoad(Row.Ty, PoisonFrameAddr, Row.Alignment,
                                                    Twine("reload.row") + std::to_string(Idx) + Twine(".") +
                                                        Row.Def->getName() + Twine("."));

    // Record the reload so we can build the phi node network and fix the frame
    // address later.
    Row.Reloads.emplace_back(CurrentReload);

    return true;
  };

  for (auto &[Suspend, Struct] : FrameStructs) {
    // For each value in the frame insert reloads, if they do not already
    // exist. Note: the location in the frame will be set when GEPs are built
    // later for now the addresses are poisoned.

    // Visit each field in the struct and create reloads as needed. Visit the
    // in reverse order to cause the reloads to occur in-order after creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<size_t>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];

      // Allocas in the frame do not require reloading
      if (Row.IsAlloca)
        continue;

      // Do not reload here if the value does not cross this suspend. Note this
      // check is needed when eviction is not used. Without eviction the frame
      // will include values that do not cross it and we should not reload the
      // value on suspends the value does not cross. That will lead to excess
      // reloading and incorrect codegen.
      if (isEvictNone() && !isSuspendCrossingValue(Row, Struct.CandidateSpills, Struct.CandidateAllocas))
        continue;

      auto &SpillUses = Struct.CandidateSpills[Row.Def];

      // Generate a reload-on-resume if the resume BB needs a reload.
      if (isReloadOnResume() || Row.ForceReloadOnResume) {
        Builder.SetInsertPoint(Struct.ResumeBB->getFirstInsertionPt());
        // Mark the reloaded BB so we don't reload it a second time
        [[maybe_unused]] auto [Itr, Inserted] = Row.ReloadedOnBB.insert(Struct.ResumeBB);

        // We should only visit the Row once per suspend for
        // reload-on=resume so Added should always be true.
        assert(Inserted);

        // Generate reload for Def
        auto *CurrentReload = Builder.CreateAlignedLoad(Row.Ty, PoisonFrameAddr, Row.Alignment,
                                                        Twine("reload.row") + std::to_string(Idx) + Twine(".") +
                                                            Row.Def->getName() + Twine("."));

        // Record the reload so we can build the phi node network and fix the frame
        // address later.
        Row.Reloads.emplace_back(CurrentReload);

        continue;
      }

      // If we didn't generate a reload-on=resume then try to generate reloads
      // on (near) each use.
      for (auto *U : SpillUses)
        InsertReloadsForUse(Struct, Row, Idx, U);
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
    if (Row.IsAlloca)
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

          // Consider codes with conditional suspends, such as the following:
          //   def A;
          //   if() {
          //     suspend 1;
          //   }
          //   suspend 2;
          //   use A
          // To mitigate potential interference it may be necessary to place
          // the spill right before each suspend. We can see that spilling
          // before suspend 2 is problematic because suspend 2 may be reached
          // by first crossing suspend 1. To be legal we reload the value after
          // suspend 1 so it can be spilled before suspend 2. In the future
          // when we have more control over splitting, we can poison the values
          // after each suspend and remove the spill from the continuation that
          // follows suspend 1. After that it will not be necessary to require
          // the value to also be reloaded-on-resume.

          assert(!isSpillOnSuspend() || isReloadOnResume() || !Row.ForceSpillOnSuspend || Row.ForceReloadOnResume);
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

// Replace poisoned frame address ptrs with computed values. Also replace
// allocas with frame address ptrs. Note, this method will split the entry
// block around the coro.begin. As a result references to entry in SpilledOnBB
// and ReloadedOnBB may be incorrect. However, at this point these structures
// should no longer be needed.
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
  for (auto &[Suspend, Struct] : FrameStructs) {
    // Visit each field in the struct and create reloads as needed. Visit the
    // fields in reverse order to cause the reloads to occur in-order after
    // creation.
    for (auto &Field : llvm::reverse(Struct.Fields)) {
      auto Idx = reinterpret_cast<size_t>(Field.Id);
      CoroFrameRow &Row = FrameTable[Idx];

      assert(Row.ResidesInSuspendFrame.contains(Suspend));
      assert(Row.Offset != OptimizedStructLayoutField::FlexibleOffset);

      auto TryReuseGep = [&](BasicBlock *BB, BasicBlock::iterator InsertPt, const Twine &Label, StringRef Name) {
        auto [Itr, Inserted] = Row.GepInBB.try_emplace(BB, nullptr);
        auto &[GepBB, GepInst] = *Itr;

        // Add a new GEP if the BB is not in the map
        if (!Inserted) {
          // Get GEP from map
          assert(GepInst);
          return GepInst;
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
        auto *Val = Builder.CreateInBoundsGEP(Shape.FrameTy, Shape.FramePtr, Idxs,
                                              Label + Twine(".addr.row") + std::to_string(Idx) + Twine(".") + Name +
                                                  Twine("."));
        // Update GepInst in Row.GepInBB
        GepInst = dyn_cast<GetElementPtrInst>(Val);

        return GepInst;
      };

      // Fix allocas that are taken over by the frame. Note that allocas that
      // do not cross suspends are not included in the FrameTable.
      if (Row.IsAlloca) {
        auto *Alloca = cast<AllocaInst>(Row.Def);
        // Insert a GEP to replace the alloca immediately after the malloc of
        // the coro frame to ensure all accesses are dominated by the GEP.
        // Insert at the end of the spill block.
        auto *GepInst =
            TryReuseGep(SpillBlock, SpillBlock->getTerminator()->getIterator(), Twine("alloca"), Alloca->getName());

        // Note: that the location of the GEP is not be the same as that of
        // the alloca. The GEP is put into the SpillBlock. The SpillBlock is
        // the entry point of each continuation, so any instrs put there will
        // be available to all continuations after the main function is split.
        compilerutils::replaceAllPointerUses(Alloca, GepInst, DeadInstructions);

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

// This method scans through the fields (crossing values) in the layout of each
// suspend and checks if they interfere any other fields in the same frame.
// Note, a value will occupy the layout (frame) of each suspend it crosses,
// consequently it is not necessary to check for interference between pairs of
// frames. In fact, when values are evicted and overwritten by other values
// overlapping memory ranges will occur.
[[maybe_unused]] void checkForValidLayout(const CoroFrameTableTy &FrameTable,
                                          const ArrayRef<OptimizedStructLayoutField> &StructFields) {
  for (auto Itr = StructFields.begin(); Itr != StructFields.end(); Itr++) {
    auto &Field = *Itr;
    if (Field.Offset == OptimizedStructLayoutField::FlexibleOffset)
      llvm_unreachable("Field must have an offset at this point.");

    auto Idx = reinterpret_cast<size_t>(Field.Id);
    auto &Row = FrameTable[Idx];

    // Check all other fields in this frame for overlap.
    for (auto OtherItr = std::next(Itr); OtherItr != StructFields.end(); OtherItr++) {
      auto &OtherField = *OtherItr;

      [[maybe_unused]] auto OtherIdx = reinterpret_cast<size_t>(OtherField.Id);
      assert(Idx != OtherIdx);

      if (Row.compareRanges({OtherField.Offset, OtherField.Size}) == 0) {
        LLVM_DEBUG(dbgs() << "Error: Overlapping fields Row " << Idx << " and Row " << OtherIdx << "\n");
        llvm_unreachable("Fields in a struct must not overlap");
      }
    }
  }
}

bool hasPoisonOperand(Instruction *I) {
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

[[maybe_unused]] void collectInstWithPoison(Function &F, SmallSet<Instruction *, 16> &PoisonInstructions) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      // Record the instruction if it has a poison operand
      if (hasPoisonOperand(&I)) {
        PoisonInstructions.insert(&I);
      }
    }
  }
}

[[maybe_unused]] bool hasNewPoisonOperand(Function &F, const SmallSet<Instruction *, 16> &PoisonInstructions) {
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
    if (Row.IsAlloca)
      continue;

    TotalGeps += Row.GepInBB.size();
    TotalReloads += Row.Reloads.size();
    TotalSpills++;

    for (auto &[Suspend, Struct] : FrameStructs) {
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

template <typename InstType> unsigned countInstrs(const Function &F) {
  unsigned Total = 0;
  for (auto &BB : F)
    for (auto &I : BB)
      if (isa<InstType>(&I))
        Total++;
  return Total;
}

// Report absolute number of new geps, spills and reloads inserted by the
// continuation state builder.
void reportGepsSpillsAndReloads(Function &F, unsigned NonFrameGeps, unsigned NonSpillStores, unsigned NonReloadLoads) {
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

void ContStateBuilderImpl::unusedValueAnalysis(const CoroFrameTableTy &FrameTable,
                                               const coro::SpillInfo &CandidateSpills,
                                               const SmallVectorImpl<coro::AllocaInfo> &CandidateAllocas) const {
  [[maybe_unused]] unsigned AllocaUnusedBytes = 0;
  [[maybe_unused]] unsigned SpillUnusedBytes = 0;

  // Analyze and report on the type of values that are unused in the current frame.
  for (auto &Row : FrameTable) {
    if (auto *DefAlloca = dyn_cast<AllocaInst>(Row.Def)) {
      auto II = std::find_if(CandidateAllocas.begin(), CandidateAllocas.end(),
                             [DefAlloca](const coro::AllocaInfo &AI) { return AI.Alloca == DefAlloca; });
      if (II == CandidateAllocas.end()) {
        // Did not find Def in CandidateAllocas for this Suspend, evict it.
        AllocaUnusedBytes += Row.Size;
        continue;
      }
    } else if (!CandidateSpills.contains(Row.Def)) {
      // Did not find Def in CandidateSpills for this Suspend, evict it.
      SpillUnusedBytes += Row.Size;
      continue;
    }
  }

  LLVM_DEBUG(dbgs() << "\tUnused Alloca Bytes: " << AllocaUnusedBytes << "\n");
  LLVM_DEBUG(dbgs() << "\tUnused Spill Bytes: " << SpillUnusedBytes << "\n");
}

// Print a depiction of the frame occupancy. For example"[+++++___|++] (12 unused Bytes)"
void ContStateBuilderImpl::fragmentationAnalysis(const CoroFrameTableTy &FrameTable,
                                                 SmallVectorImpl<OptimizedStructLayoutField> &StructFields,
                                                 const coro::SpillInfo &CandidateSpills,
                                                 const SmallVectorImpl<coro::AllocaInfo> &CandidateAllocas) const {
  // Start memory allocation depiction with '['
  dbgs() << "[";

  // This scans through assigned memory starting at LastOffset=0, keeping
  // track of the start of each gap with GapStart and adding gaps between
  // fields in the struct to the GapList. The fields are kept in sorted order.
  uint64_t LastOffset = 0;
  uint64_t GapStart = 0;
  uint64_t TotalGapBytes = 0; // Track the total number of Gap bytes.
  for (auto &Field : StructFields) {
    // Get next field in the struct
    auto Idx = reinterpret_cast<size_t>(Field.Id);
    auto &Row = FrameTable[Idx];

    // Compute Gap bytes
    uint64_t GapBytes = Row.Offset - GapStart;
    TotalGapBytes += GapBytes;

    // If field is 'used' we will not ignore it later.
    bool Used = isSuspendCrossingValue(Row, CandidateSpills, CandidateAllocas);

    assert(Row.Offset != OptimizedStructLayoutField::FlexibleOffset);

    // Found a field that is used at this suspend. If the GapStart is less
    // than the start of the field then we found a gap, so we insert it into
    // GapList.
    if (Used) {
      // Move GapStart to the end of this field.
      GapStart = Row.Offset + Row.Size;
    }

    // Scan from LastOffset to the end of this field. If LastOffset is less
    // than the field's start offset Row.Offset, then we print a '_' to
    // indicate a gap in the struct, otherwise we print a '.'. The LastOffset
    // is incremented by 4. TODO: modify the print method to indicate if the
    // dword is partially filled by tracking each dword half separately.
    // In general this is most useful when the frame packing has very large
    // gaps, i.e. larger than a single dword, so partially filled dwords are
    // not all that important to render. A '|' is printed every 32 bytes, 4
    // dwords, to make it easier to compare two different frame packings.
    while (LastOffset < Row.Offset + Row.Size) {
      if (LastOffset % 32 == 0 && LastOffset != 0)
        dbgs() << "|";
      if (LastOffset < Row.Offset || !Used)
        dbgs() << "_";
      else
        dbgs() << "+";
      LastOffset += 4;
    }
  }

  // End memory allocation depiction with ']'
  dbgs() << "] (" << TotalGapBytes << " unused bytes)\n";
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

  // ======== Init Loops ========

  // Ensure no gaps in the block numbers.
  F.renumberBlocks();

  // These analysis results cannot be reused from an earlier pass. The analysis
  // must be done here because CoroSplit invalidates the info during
  // normalization by breaking critical edges. The results cannot be reused for
  // a later pass because CoroSplit will split the Function into ramp and
  // resume continuations. Consequently, we just do the analysis here and
  // forget about the results after buildCoroutineFrame is done.
  DominatorTree DT(F);
  LoopInfo LI(DT);

  createLoopPreHeadersIfMissing(DT, LI);

  // Note: No new blocks should be inserted past this point until we call
  // createFrameGEPs that will split the entry block. Doing so will affect
  // block numbering, analysis results (DT, LI) as well as instr->BB maps.

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

  // ======== Frame Structs ========

  auto Id = Shape.getRetconCoroId();
  auto RetconSize = Id->getStorageSize();
  auto RetconAlign = Id->getStorageAlignment();

  LLVM_DEBUG({
    dbgs() << "----- Frame Data At Each Suspend -----\n";
    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    dbgs() << "Function: " << F.getName() << " (" << Stage << ")\n";
    dbgs() << "Total # of Suspends: " << FrameStructs.size() << "\n";
  });

  for (auto [Idx, Frame] : llvm::enumerate(FrameStructs)) {
    auto &[Suspend, Struct] = Frame;
    LLVM_DEBUG(dbgs() << "Suspend " << Idx << "\n");

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
  }

  // ======== Frame Layout ========

  // Stacklifetime analyzer is used to avoid interference when an alloca is
  // overwriting an existing alloca in the frame. Other cases are currently
  // handled by modifying the spilling and/or reloading locations to avoid
  // potential interference. This is done by computeInterference that sets the
  // previous row's MustReloadOnResume or the current row's MustSpillOnSuspend
  // flags.
  SmallVector<AllocaInst *, 4> AllAllocas;
  for (auto &I : FrameStructs) {
    auto &Struct = I.second;
    AllAllocas.reserve(AllAllocas.size() + Struct.CandidateAllocas.size());
    for (const auto &AI : Struct.CandidateAllocas)
      AllAllocas.push_back(AI.Alloca);
  }

  StackLifetime StackLifetimeAnalyzer(F, AllAllocas, StackLifetime::LivenessType::May);
  StackLifetimeAnalyzer.run();

  LLVM_DEBUG({
    dbgs() << "----- Frame Layout At Each Suspend -----\n";
    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    dbgs() << "Function: " << F.getName() << " (" << Stage << ")\n";
    dbgs() << "Total # of Suspends: " << FrameStructs.size() << "\n";
  });

  for (auto [Idx, Row] : llvm::enumerate(FrameStructs)) {
    auto &[Suspend, Struct] = Row;
    LLVM_DEBUG(dbgs() << "Suspend " << Idx << "\n");

    // Initialize the fields with pre-existing offsets and identify the gaps
    // in the current frame layout.
    SmallVector<Gap, 8> Gaps;
    initFrameStructLayout(Gaps, Suspend, Struct);

    // Frag analysis must be done after initFrameStructLayout because this will
    // ensure the frame is sorted.
    LLVM_DEBUG({
      dbgs() << "\tInitial Frame Occupancy: ";
      fragmentationAnalysis(FrameTable, Struct.Fields, Struct.CandidateSpills, Struct.CandidateAllocas);
    });

    // Compute struct layouts
    computeFrameStructLayoutGreedy(Suspend, Gaps, StackLifetimeAnalyzer);

    // Sorting fields by offset and determine the total frame size required.
    finalizeFrameStructLayout(Struct);

    // Unused value and frag analysis must be done after computing the frame
    // layout because we need the frame to be populated for unused value
    // analysis and we need the offsets in the frame for frag analysis.
    LLVM_DEBUG({
      dbgs() << "\tFinal Frame Occupancy:   ";
      fragmentationAnalysis(FrameTable, Struct.Fields, Struct.CandidateSpills, Struct.CandidateAllocas);
      unusedValueAnalysis(FrameTable, Struct.CandidateSpills, Struct.CandidateAllocas);
      dbgs() << "\tFrame Size Bytes: " << Struct.Size << "\n";
      dbgs() << "\tFrame Align Bytes: " << Struct.Alignment.value() << "\n";
    });

    if (isEvictUnused()) {
      // Determine if there is any interference due to reuse of space in the
      // frame and specify a spill/reload strategy accordingly. If space is not
      // reused then interference cannot occur.
      computeInterference(Struct.Fields);
    }
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
  insertReloads(DT);

  // ======== Insert Spills ========
  // Spills are done after reloads so we can try to insert spills after
  // last-uses (reloads) when eviction is enabled.

  LLVM_DEBUG(dbgs() << "Inserting Spills\n");

  insertSpills(Shape, DT, LI);

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

  LLVM_DEBUG({
    dbgs() << "-- FrameStructs --\n";
    unsigned Idx = 0;
    for (auto &[Suspend, Struct] : FrameStructs) {
      dbgs() << "Suspend " << Idx++ << "\n";
      dbgs() << "\tSuspendInst: ";
      Suspend->dump();
      dbgs() << "\tSuspendBB: %" << compilerutils::bb::getLabel(Suspend->getParent()) << "\n";
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

#ifndef NDEBUG
  // ======== Sanity Checks ========
  // Verify all fields in the frame are valid. Invalid fields do not have a
  // valid offset, or have a range that overlaps with other fields.
  for (auto &[Suspend, Struct] : FrameStructs)
    checkForValidLayout(FrameTable, Struct.Fields);

#endif

  LLVM_DEBUG(dbgs() << "Creating GEPs\n");

  // Build GEPs to complete the access to the frame structs.
  createFrameGEPs(DeadInstructions);

#ifndef NDEBUG
  // ======== Poison instructions ========
  // Verify no new poisons are left in the IR
  if (hasNewPoisonOperand(F, PoisonInstructions)) {
    llvm_unreachable("Error: Found poison");
  }
#endif

  LLVM_DEBUG(dbgs() << "Final Frame Size Bytes: " << MaxFrameSize << "\n");
  LLVM_DEBUG(dbgs() << "Final Frame Align Bytes: " << MaxFrameAlign.value() << "\n");

  LLVM_DEBUG(reportContStateInfo());

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
