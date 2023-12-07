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

//===- RegisterBuffer.cpp - Split an array into register and memory -------===//
//
// A pass that converts a global to be partially buffered in registers and spill
// to memory.
//
// This pass handles all globals marked with registerbuffer metadata:
// @GLOBAL = external global [20 x i32], !registerbuffer !1
// !1 = !{ i32 15 }
//
// The global has to be an array. The registerbuffer metadata contains a
// single i32 that specifies the number of buffered array elements that are
// in registers.
// In the previous example, 15 elements are put into registers. The user of this
// pass is responsible for saving the pointer to the memory region that stores
// the rest of the elements, which will contain element 15 to 20 in this
// example.
//
// The result will be a smaller global, representing the register part:
// @GLOBAL = external addrspace(20) global [15 x i32]
//
// After the buffer is lowered, the memory pointer is accessed
// through the intrinsics
// i32 addrspace(21)* @registerbuffer.getpointer.a20i32([20 x i32]
//   addrspace(20)*)
// A later pass needs to find these and change them to the actual memory
// pointer.
//
// For changing a global access to accessing the memory pointer, all GEPs and
// casts are copied to use (getpointer() - sizeof(GLOBAL)) as the base address.
// This ensures that the correct offset will be reached, no matter how it is
// computed.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <algorithm>
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "register-buffer"

/// Try to find a static offset of the address relative to the global.
static std::optional<uint64_t> findOffset(const DataLayout &DL,
                                          GlobalValue *Global, Value *Address) {
  // Strip casts
  while (true) {
    if (auto *C = dyn_cast<BitCastInst>(Address)) {
      Address = C->getOperand(0);
    } else if (auto *C = dyn_cast<AddrSpaceCastInst>(Address)) {
      Address = C->getOperand(0);
    } else if (auto *C = dyn_cast<ConstantExpr>(Address)) {
      if (C->getOpcode() == Instruction::BitCast)
        Address = C->getOperand(0);
      else if (C->getOpcode() == Instruction::AddrSpaceCast)
        Address = C->getOperand(0);
      else
        break;
    } else {
      break;
    }
  }

  if (Address == Global)
    return 0;

  if (auto *GEP = dyn_cast<GEPOperator>(Address)) {
    APInt Offset(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
    if (GEP->accumulateConstantOffset(DL, Offset)) {
      if (auto O = findOffset(DL, Global, GEP->getPointerOperand()))
        return Offset.getZExtValue() + *O;
    }
  }
  return {};
}

static Function *getRegisterBufferGetPointer(Module &M,
                                             Type *RegisterBufferType,
                                             unsigned Addrspace) {
  SmallVector<char, 33> StrBuf; // 32 chars for a double digit array
  auto *ElemTy = RegisterBufferType->getArrayElementType();
  uint64_t RegisterBufferSize = RegisterBufferType->getArrayNumElements();
  uint64_t IntSize = ElemTy->getPrimitiveSizeInBits();
  auto Name = (Twine("registerbuffer.getpointer.a") +
               Twine(RegisterBufferSize) + "i" + Twine(IntSize))
                  .toStringRef(StrBuf);
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::WillReturn});
  auto *Func = cast<Function>(
      M.getOrInsertFunction(
           Name, AL, ElemTy->getPointerTo(Addrspace),
           RegisterBufferType->getPointerTo(GlobalRegisterAddrspace))
          .getCallee());
  Func->setOnlyReadsMemory();
  return Func;
}

/// Return a pointer to the memory region by getting the memory address from the
/// intrinsic and subtracting the size of the global.
static Value *getMemoryPtr(IRBuilder<> &Builder, GlobalValue *Global,
                           uint64_t RegisterCount, unsigned Addrspace) {
  auto *BufferTy = Global->getValueType();
  auto *Ty = BufferTy->getArrayElementType();
  auto *GetPtr =
      getRegisterBufferGetPointer(*Global->getParent(), BufferTy, Addrspace);
  Value *MemPtr = Builder.CreateCall(GetPtr, {Global});
  MemPtr = Builder.CreateGEP(Ty, MemPtr, Builder.getInt32(-RegisterCount));
  return Builder.CreateBitCast(
      MemPtr, Global->getValueType()->getPointerTo(
                  MemPtr->getType()->getPointerAddressSpace()));
}

static Instruction *createLoadStore(IRBuilder<> &Builder, Type *Ty,
                                    Value *StoreVal, Value *Address,
                                    Align Alignment, AAMDNodes AATags,
                                    bool IsLoad) {
  Instruction *LoadStore;
  if (IsLoad)
    LoadStore = Builder.CreateAlignedLoad(Ty, Address, Alignment);
  else
    LoadStore = Builder.CreateAlignedStore(StoreVal, Address, Alignment);
  if (AATags)
    LoadStore->setAAMetadata(AATags);
  return LoadStore;
}

Value *RegisterBufferPass::computeMemAddr(IRBuilder<> &Builder,
                                          Value *Address) {
  if (Address == Global)
    return getMemoryPtr(Builder, Global, Data.RegisterCount, Data.Addrspace);

  if (MemAccessors.count(Address))
    return MemAccessors[Address];

  IRBuilder<>::InsertPointGuard Guard(Builder);

  // Do not cache constant expressions, we don't know where in the code they
  // need to be duplicated
  bool DoCache = false;
  if (auto *Inst = dyn_cast<Instruction>(Address)) {
    DoCache = true;
    Builder.SetInsertPoint(Inst);
  }

  Value *New;
  if (auto *Inst = dyn_cast<GEPOperator>(Address)) {
    auto *Src = Inst->getPointerOperand();
    Value *MemSrc = computeMemAddr(Builder, Src);
    // Clone instruction without inbounds (may be out-of-bounds in memory for
    // the register part)
    SmallVector<Value *> Indices(Inst->indices());
    New = Builder.CreateGEP(Inst->getSourceElementType(), MemSrc, Indices);
  } else if (auto *Inst = dyn_cast<CastInst>(Address)) {
    auto *Src = Inst->getOperand(0);
    Value *MemSrc = computeMemAddr(Builder, Src);
    New = Builder.CreateCast(
        Inst->getOpcode(), MemSrc,
        getWithSamePointeeType(cast<PointerType>(Inst->getDestTy()),
                               Data.Addrspace));
  } else if (auto *Inst = dyn_cast<ConstantExpr>(Address)) {
    if (Inst->isCast()) {
      auto *Src = Inst->getOperand(0);
      Value *MemSrc = computeMemAddr(Builder, Src);
      New = Builder.CreateCast(
          static_cast<Instruction::CastOps>(Inst->getOpcode()), MemSrc,
          getWithSamePointeeType(cast<PointerType>(Inst->getType()),
                                 Data.Addrspace));
    } else {
      LLVM_DEBUG(Address->dump());
      llvm_unreachable(
          "Unhandled constant when rebasing pointer path to memory");
    }
  } else {
    LLVM_DEBUG(Address->dump());
    llvm_unreachable(
        "Unhandled instruction when rebasing pointer path to memory");
  }

  if (DoCache)
    MemAccessors[Address] = New;
  return New;
}

Value *RegisterBufferPass::handleSingleLoadStore(
    IRBuilder<> &Builder, Type *Ty, Value *StoreVal, Value *Address,
    Align Alignment, AAMDNodes AATags, bool IsLoad) {
  LLVM_DEBUG(dbgs() << "register buffer: Check address " << *Address << "\n");
  assert(IsLoad != (!!StoreVal) && "Expected either IsLoad or StoreVal");

  const DataLayout &DL = Global->getParent()->getDataLayout();
  std::optional<uint64_t> Offset = findOffset(DL, Global, Address);

#ifndef NDEBUG
  // Check if the offset is out-of-bounds
  uint32_t ElementSize = DL.getTypeStoreSize(ElementType);
  if (Offset && (*Offset / ElementSize) >= TotalElementCount) {
    dbgs() << "Out-of-bounds access at index " << *Offset << " into global "
           << *Global << " with total size " << TotalElementCount << "\n";
    llvm_unreachable("Out-of-bounds register buffer access");
  }
#endif

  // Change load/store to use addrspace(20)
  auto *AddressType = cast<PointerType>(Address->getType());
  Address = Builder.CreateAddrSpaceCast(
      Address, getWithSamePointeeType(AddressType, GlobalRegisterAddrspace));

  // If only registers are accessed, emit a simple load/store
  if (TotalElementCount <= Data.RegisterCount)
    return createLoadStore(Builder, Ty, StoreVal, Address, Alignment, AATags,
                           IsLoad);

  // If the offset is known, emit a load/store statically
  if (Offset) {
    LLVM_DEBUG(dbgs() << "register buffer: Found constant offset: "
                      << Offset.value() << "\n");
    uint64_t ElementSize = ElementType->getPrimitiveSizeInBits() / 8;
    const uint32_t Index = Offset.value() / ElementSize;
    if (Index < Data.RegisterCount) {
      LLVM_DEBUG(dbgs() << "register buffer: " << Index << " < "
                        << Data.RegisterCount << "  =>  register\n");
      // Access goes into the register part
      return createLoadStore(Builder, Ty, StoreVal, Address, Alignment, AATags,
                             IsLoad);
    }
    LLVM_DEBUG(dbgs() << "register buffer: " << Index
                      << " >= " << Data.RegisterCount << "  =>  memory\n");

    // Get memory address
    auto *Addr = computeMemAddr(Builder, Address);

    // Convert to load from memory
    return createLoadStore(Builder, Ty, StoreVal, Addr, Alignment, AATags,
                           IsLoad);
  }
  LLVM_DEBUG(dbgs() << "register buffer: Found dynamic offset\n");

  // Add a dynamic switch based on the address
  auto *GlobalInt = Builder.CreatePtrToInt(Global, Builder.getInt32Ty());
  auto *AddressInt = Builder.CreatePtrToInt(Address, Builder.getInt32Ty());
  auto *Difference = Builder.CreateSub(AddressInt, GlobalInt);
  uint64_t RegistersByteCount =
      DL.getTypeStoreSize(Global->getValueType()).getFixedValue();

  Instruction *InsertI = &*Builder.GetInsertPoint();
  auto ResetInsertPoint = make_scope_exit(
      [InsertI, &Builder]() { Builder.SetInsertPoint(InsertI); });

  Instruction *Then;
  Instruction *Else;
  auto *Cond =
      Builder.CreateICmpULT(Difference, Builder.getInt32(RegistersByteCount));
  SplitBlockAndInsertIfThenElse(Cond, InsertI, &Then, &Else);
  BasicBlock *TailBB = InsertI->getParent();
  BasicBlock *ThenBB = Then->getParent();

  // Access goes into the register part
  Builder.SetInsertPoint(Then);
  Instruction *ThenLoadStore = createLoadStore(Builder, Ty, StoreVal, Address,
                                               Alignment, AATags, IsLoad);

  // Not in the register range
  auto *Addr = computeMemAddr(Builder, Address);
  Builder.SetInsertPoint(Else);

  Instruction *ElseLoadStore =
      createLoadStore(Builder, Ty, StoreVal, Addr, Alignment, AATags, IsLoad);
  if (IsLoad) {
    Builder.SetInsertPoint(&*TailBB->getFirstInsertionPt());
    auto *PHI = Builder.CreatePHI(Ty, 2);
    PHI->addIncoming(ThenLoadStore, ThenBB);
    PHI->addIncoming(ElseLoadStore, ElseLoadStore->getParent());
    return PHI;
  }
  return ElseLoadStore;
}

namespace {

/// Generic recursive split emission class.
/// The OpSplitter originates from the SROA pass and is extended to split
/// integers into smaller sizes.
template <typename Derived> class OpSplitter {
protected:
  /// The builder used to form new instructions.
  IRBuilder<> IRB;

  /// The indices which to be used with insert- or extractvalue to select the
  /// appropriate value within the aggregate.
  SmallVector<unsigned, 4> Indices;

  /// The indices to a GEP instruction which will move Ptr to the correct slot
  /// within the aggregate.
  SmallVector<Value *, 4> GEPIndices;

  /// The base pointer of the original op, used as a base for GEPing the
  /// split operations.
  Value *Ptr;

  /// The base pointee type being GEPed into.
  Type *BaseTy;

  /// Known alignment of the base pointer.
  Align BaseAlign;

  /// To calculate offset of each component so we can correctly deduce
  /// alignments.
  const DataLayout &DL;

  /// Initialize the splitter with an insertion point, Ptr and start with a
  /// single zero GEP index.
  OpSplitter(Instruction *InsertionPoint, Value *Ptr, Type *BaseTy,
             Align BaseAlign, const DataLayout &DL)
      : IRB(InsertionPoint), GEPIndices(1, IRB.getInt32(0)), Ptr(Ptr),
        BaseTy(BaseTy), BaseAlign(BaseAlign), DL(DL) {}

public:
  /// Parts of a load that is split in multiple int loads.
  struct LoadStorePart {
    // In Bytes
    uint64_t Offset;
    // In Bytes
    uint64_t Size;
  };

  /// Generic recursive split emission routine.
  ///
  /// This method recursively splits an aggregate op (load or store) into
  /// scalar or vector ops. It splits recursively until it hits a single value
  /// and emits that single value operation via the template argument.
  ///
  /// The logic of this routine relies on GEPs and insertvalue and
  /// extractvalue all operating with the same fundamental index list, merely
  /// formatted differently (GEPs need actual values).
  ///
  /// \param Ty  The type being split recursively into smaller ops.
  /// \param Agg The aggregate value being built up or stored, depending on
  /// whether this is splitting a load or a store respectively.
  void emitSplitOps(Type *Ty, Value *&Agg, const Twine &Name) {
    if (Ty->isSingleValueType()) {
      unsigned Offset = DL.getIndexedOffsetInType(BaseTy, GEPIndices);
      Align Alignment = commonAlignment(BaseAlign, Offset);

      // Split too large and unaligned values
      // Load the single value and insert it using the indices.
      uint64_t Size = DL.getTypeStoreSize(Ty).getFixedValue();
      // Split types bigger than a register
      uint64_t SingleSize =
          std::min(Size, static_cast<uint64_t>(RegisterBytes));
      // Split unaligned types into byte
      if (Alignment.value() < SingleSize)
        SingleSize = 1;

      if (SingleSize < Size) {
        // Use a packed struct to describe and load all the parts
        SmallVector<Type *> Elements;
        // Split load
        uint64_t Offset = 0;
        while (Offset < Size) {
          uint64_t ThisPartSize = std::min(SingleSize, Size - Offset);
          Elements.push_back(IRB.getIntNTy(ThisPartSize * 8));

          Offset += ThisPartSize;
        }
        auto *StructTy = StructType::get(IRB.getContext(), Elements, true);
        return static_cast<Derived *>(this)->emitFunc(Ty, Agg, StructTy,
                                                      Alignment, Name);
      }

      return static_cast<Derived *>(this)->emitFunc(Ty, Agg, nullptr, Alignment,
                                                    Name);
    }

    if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
      unsigned OldSize = Indices.size();
      (void)OldSize;
      for (unsigned Idx = 0, Size = ATy->getNumElements(); Idx != Size; ++Idx) {
        assert(Indices.size() == OldSize && "Did not return to the old size");
        Indices.push_back(Idx);
        GEPIndices.push_back(IRB.getInt32(Idx));
        emitSplitOps(ATy->getElementType(), Agg, Name + "." + Twine(Idx));
        GEPIndices.pop_back();
        Indices.pop_back();
      }
      return;
    }

    if (StructType *STy = dyn_cast<StructType>(Ty)) {
      unsigned OldSize = Indices.size();
      (void)OldSize;
      for (unsigned Idx = 0, Size = STy->getNumElements(); Idx != Size; ++Idx) {
        assert(Indices.size() == OldSize && "Did not return to the old size");
        Indices.push_back(Idx);
        GEPIndices.push_back(IRB.getInt32(Idx));
        emitSplitOps(STy->getElementType(Idx), Agg, Name + "." + Twine(Idx));
        GEPIndices.pop_back();
        Indices.pop_back();
      }
      return;
    }

    llvm_unreachable("Only arrays and structs are aggregate loadable types");
  }
};

struct LoadOpSplitter : public OpSplitter<LoadOpSplitter> {
  RegisterBufferPass *Pass;
  AAMDNodes AATags;

  LoadOpSplitter(RegisterBufferPass *Pass, Instruction *InsertionPoint,
                 Value *Ptr, Type *BaseTy, AAMDNodes AATags, Align BaseAlign,
                 const DataLayout &DL)
      : OpSplitter<LoadOpSplitter>(InsertionPoint, Ptr, BaseTy, BaseAlign, DL),
        Pass(Pass), AATags(AATags) {}

  /// Emit a leaf load of a single value. This is called at the leaves of the
  /// recursive emission to actually load values.
  void emitFunc(Type *Ty, Value *&Agg, StructType *Parts, Align Alignment,
                const Twine &Name) {
    assert(Ty->isSingleValueType());
    // Load the single value and insert it using the indices.
    Value *GEP = IRB.CreateInBoundsGEP(BaseTy, Ptr, GEPIndices, Name + ".gep");

    unsigned Offset = DL.getIndexedOffsetInType(BaseTy, GEPIndices);
    Value *Load = nullptr;
    if (Parts) {
      Load = PoisonValue::get(Parts);
      Value *ElemTyPtr =
          IRB.CreateBitCast(GEP, Parts->getPointerTo(), Name + ".ptr");

      // A struct cannot be cast into an integer, so we store it in an alloca
      // and cast the pointer instead. The packed struct may have padding and a
      // greater store size, ignore that.
      assert(DL.getTypeStoreSize(Ty).getFixedValue() <=
                 DL.getTypeStoreSize(Parts) &&
             "Type sizes do not match");
      // Load parts
      for (unsigned PartI = 0; PartI < Parts->getStructNumElements(); PartI++) {
        auto *Part = Parts->getStructElementType(PartI);
        Value *PtrI = IRB.CreateConstInBoundsGEP2_32(
            Parts, ElemTyPtr, 0, PartI, Name + ".gep." + Twine(PartI));

        APInt FieldOffsetInt(
            DL.getIndexSizeInBits(PtrI->getType()->getPointerAddressSpace()),
            0);
        bool FieldOffsetSuccess = GEPOperator::accumulateConstantOffset(
            Parts, {IRB.getInt64(0), IRB.getInt64(PartI)}, DL, FieldOffsetInt);
        assert(FieldOffsetSuccess &&
               "Failed to compute field offset of packed struct");
        (void)FieldOffsetSuccess;
        uint64_t FieldOffset = FieldOffsetInt.getZExtValue();

        Value *LoadStorePart = Pass->handleSingleLoadStore(
            IRB, Part, nullptr, PtrI, commonAlignment(Alignment, FieldOffset),
            AATags ? AATags.shift(Offset + FieldOffset) : AATags, true);

        // Insert into struct
        Load = IRB.CreateInsertValue(Load, LoadStorePart, {PartI},
                                     Name + ".insert." + Twine(PartI));
      }

      auto *InsertPoint = &*IRB.GetInsertPoint();
      IRB.SetInsertPointPastAllocas(InsertPoint->getFunction());
      auto *Alloca = IRB.CreateAlloca(Parts, nullptr, Name + ".alloca");
      IRB.SetInsertPoint(InsertPoint);

      IRB.CreateStore(Load, Alloca);
      auto *CastTyPtr =
          IRB.CreateBitCast(Alloca, Ty->getPointerTo(Alloca->getAddressSpace()),
                            Name + ".alloca.cast");
      Load = IRB.CreateAlignedLoad(Ty, CastTyPtr, Alloca->getAlign(),
                                   Name + ".alloca.load");
    } else {
      Load = Pass->handleSingleLoadStore(IRB, Ty, nullptr, GEP, Alignment,
                                         AATags ? AATags.shift(Offset) : AATags,
                                         true);
    }

    if (Agg->getType()->isAggregateType())
      Agg = IRB.CreateInsertValue(Agg, Load, Indices, Name + ".insert");
    else
      Agg = Load;
    LLVM_DEBUG(dbgs() << "          to: " << *Load << "\n");
  }
};

struct StoreOpSplitter : public OpSplitter<StoreOpSplitter> {
  RegisterBufferPass *Pass;
  AAMDNodes AATags;

  StoreOpSplitter(RegisterBufferPass *Pass, Instruction *InsertionPoint,
                  Value *Ptr, Type *BaseTy, AAMDNodes AATags, Align BaseAlign,
                  const DataLayout &DL)
      : OpSplitter<StoreOpSplitter>(InsertionPoint, Ptr, BaseTy, BaseAlign, DL),
        Pass(Pass), AATags(AATags) {}

  /// Emit a leaf store of a single value. This is called at the leaves of the
  /// recursive emission to actually produce stores.
  void emitFunc(Type *Ty, Value *&Agg, StructType *Parts, Align Alignment,
                const Twine &Name) {
    assert(Ty->isSingleValueType());
    // Extract the single value and store it using the indices.
    //
    // The gep and extractvalue values are factored out of the CreateStore
    // call to make the output independent of the argument evaluation order.
    Value *Val;
    if (Agg->getType()->isAggregateType())
      Val = IRB.CreateExtractValue(Agg, Indices, Name + ".extract");
    else
      Val = Agg;

    Value *GEP = IRB.CreateInBoundsGEP(BaseTy, Ptr, GEPIndices, Name + ".gep");

    unsigned Offset = DL.getIndexedOffsetInType(BaseTy, GEPIndices);
    if (Parts) {
      Value *ElemTyPtr =
          IRB.CreateBitCast(GEP, Parts->getPointerTo(), Name + ".ptr");

      // A struct cannot be cast from an integer, so we store it in an alloca
      // and cast the pointer instead. The packed struct may have padding a a
      // greater store size, ignore that.
      assert(DL.getTypeStoreSize(Ty).getFixedValue() <=
                 DL.getTypeStoreSize(Parts) &&
             "Type sizes do not match");
      auto *InsertPoint = &*IRB.GetInsertPoint();
      IRB.SetInsertPointPastAllocas(InsertPoint->getFunction());
      auto *Alloca = IRB.CreateAlloca(Parts, nullptr, Name + ".alloca");
      IRB.SetInsertPoint(InsertPoint);

      auto *CastTyPtr =
          IRB.CreateBitCast(Alloca, Ty->getPointerTo(Alloca->getAddressSpace()),
                            Name + ".alloca.cast");
      IRB.CreateAlignedStore(Val, CastTyPtr, Alloca->getAlign());
      Value *CastVal = IRB.CreateLoad(Parts, Alloca, Name + ".alloca.load");

      // Store parts
      for (unsigned PartI = 0; PartI < Parts->getStructNumElements(); PartI++) {
        auto *Part = Parts->getStructElementType(PartI);
        Value *PtrI = IRB.CreateConstInBoundsGEP2_32(
            Parts, ElemTyPtr, 0, PartI, Name + ".gep." + Twine(PartI));

        Value *ThisVal = IRB.CreateExtractValue(
            CastVal, {PartI}, Name + ".extract." + Twine(PartI));

        APInt FieldOffsetInt(
            DL.getIndexSizeInBits(PtrI->getType()->getPointerAddressSpace()),
            0);
        bool FieldOffsetSuccess = GEPOperator::accumulateConstantOffset(
            Parts, {IRB.getInt64(0), IRB.getInt64(PartI)}, DL, FieldOffsetInt);
        assert(FieldOffsetSuccess &&
               "Failed to compute field offset of packed struct");
        (void)FieldOffsetSuccess;
        uint64_t FieldOffset = FieldOffsetInt.getZExtValue();

        Pass->handleSingleLoadStore(
            IRB, Part, ThisVal, PtrI, commonAlignment(Alignment, FieldOffset),
            AATags ? AATags.shift(Offset + FieldOffset) : AATags, false);
      }
    } else {
      Pass->handleSingleLoadStore(IRB, Ty, Val, GEP, Alignment,
                                  AATags ? AATags.shift(Offset) : AATags,
                                  false);
    }

    LLVM_DEBUG(dbgs() << "          to: Store\n");
  }
};

} // namespace

RegisterBufferPass::RegisterBufferPass() {}

/// Compute the adjusted alignment for a load or store from an offset.
static Align getAdjustedAlignment(Instruction *I, uint64_t Offset) {
  return commonAlignment(getLoadStoreAlignment(I), Offset);
}

/// Either stores StoreVal or returns the result from a load.
void RegisterBufferPass::handleLoadStore(IRBuilder<> &Builder, Instruction *I,
                                         Value *Address, bool IsLoad) {
  LLVM_DEBUG(dbgs() << "register buffer: Check address " << *Address << "\n");

  // Split usages of structs/arrays, unaligned loads/stores, and loads/stores
  // bigger than the register size.
  const auto &DL = I->getModule()->getDataLayout();
  if (IsLoad) {
    LoadOpSplitter Splitter(this, I, cast<LoadInst>(I)->getPointerOperand(),
                            I->getType(), I->getAAMetadata(),
                            getAdjustedAlignment(I, 0), DL);
    Value *V = PoisonValue::get(I->getType());
    Splitter.emitSplitOps(I->getType(), V, I->getName() + ".fca");
    I->replaceAllUsesWith(V);
    I->eraseFromParent();
  } else {
    auto *SI = cast<StoreInst>(I);
    Value *V = SI->getValueOperand();
    StoreOpSplitter Splitter(this, I, SI->getPointerOperand(), V->getType(),
                             I->getAAMetadata(), getAdjustedAlignment(I, 0),
                             DL);
    Splitter.emitSplitOps(V->getType(), V, V->getName() + ".fca");
    I->eraseFromParent();
  }
}

llvm::PreservedAnalyses
RegisterBufferPass::run(llvm::Module &M,
                        llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass register-buffer\n");

  MemAccessors.clear();
  IRBuilder<> Builder(M.getContext());

  bool Changed = false;

  for (auto &OldGlobal : make_early_inc_range(M.globals())) {
    const auto *MD = OldGlobal.getMetadata("registerbuffer");
    if (!MD)
      continue;
    Changed = true;
    Data = getRegisterBufferMetadata(MD);

    // Check that the global is an [_ x i32] with a size greater than the size
    // specified in metadata.
    auto *ATy = dyn_cast<ArrayType>(OldGlobal.getValueType());
    TotalElementCount = ATy->getArrayNumElements();
    assert(ATy && "register buffer global must be an array");
    ElementType = dyn_cast<IntegerType>(ATy->getElementType());
    assert(ElementType && ElementType->getIntegerBitWidth() == 32 &&
           "register buffer global must be an array of i32");

    // Create a new global with the right size and addrspace
    auto *NewATy = ArrayType::get(
        ElementType, std::min(Data.RegisterCount, TotalElementCount));
    Global = cast<GlobalVariable>(M.getOrInsertGlobal("", NewATy, [&] {
      return new GlobalVariable(
          M, NewATy, false, GlobalVariable::ExternalLinkage, nullptr,
          OldGlobal.getName(), nullptr, GlobalVariable::NotThreadLocal,
          GlobalRegisterAddrspace);
    }));
    Global->takeName(&OldGlobal);
    Global->setUnnamedAddr(OldGlobal.getUnnamedAddr());
    Global->setVisibility(OldGlobal.getVisibility());
    Global->setThreadLocalMode(OldGlobal.getThreadLocalMode());
    Global->setDLLStorageClass(OldGlobal.getDLLStorageClass());
    Global->setPartition(OldGlobal.getPartition());
    Global->setLinkage(OldGlobal.getLinkage());

    // Replace with a bitcast to the previous addrspace
    // and gather uses.
    auto *CastNewGlobal = ConstantExpr::getPointerBitCastOrAddrSpaceCast(
        Global, OldGlobal.getType());
    OldGlobal.replaceAllUsesWith(CastNewGlobal);
    OldGlobal.eraseFromParent();

    // RAUW may fold casts, so we need to search uses of NewGlobal, not of
    // CastNewGlobal
    DenseSet<User *> UseList(Global->user_begin(), Global->user_end());
    SmallVector<Value *> UseWorklist(Global->user_begin(), Global->user_end());
    // Collect first to prevent constant expressions from being removed while we
    // iterate over them.
    SmallVector<Value *> Uses;
    while (!UseWorklist.empty()) {
      auto *Use = UseWorklist.pop_back_val();

      bool IsConstExprCast = false;
      if (auto *Const = dyn_cast<ConstantExpr>(Use)) {
        auto OpCode = Const->getOpcode();
        IsConstExprCast = OpCode == Instruction::GetElementPtr ||
                          OpCode == Instruction::BitCast ||
                          OpCode == Instruction::AddrSpaceCast;
      }

      if (isa<GetElementPtrInst>(Use) || isa<BitCastInst>(Use) ||
          IsConstExprCast) {
        for (auto *U : Use->users()) {
          if (!UseList.count(U)) {
            UseList.insert(U);
            UseWorklist.push_back(U);
          } else {
            LLVM_DEBUG(dbgs() << "Already there " << *U << "\n");
          }
        }
      } else if (isa<LoadInst>(Use) || isa<StoreInst>(Use) ||
                 isa<CallInst>(Use)) {
        Uses.push_back(Use);
      } else {
        LLVM_DEBUG(dbgs() << "Failed to handle use of global: " << *Use
                          << "\n");
        llvm_unreachable("Failed to handle global use");
      }
    }

    // Go through all uses and handle loads, stores and intrinsic calls
    for (auto *Use : Uses) {
      LLVM_DEBUG(dbgs() << "Handle use " << *Use << "\n");

      if (auto *I = dyn_cast<LoadInst>(Use)) {
        handleLoadStore(Builder, I, I->getPointerOperand(), true);
      } else if (auto *I = dyn_cast<StoreInst>(Use)) {
        handleLoadStore(Builder, I, I->getPointerOperand(), false);
      } else if (auto *I = dyn_cast<CallInst>(Use)) {
        if (auto *Intr = I->getCalledFunction()) {
          // Handle intrinsics
          auto Name = Intr->getName();
          // Ignore registerbuffer.setpointerbarrier barriers but leave them in
          // the code
          if (Name.startswith("registerbuffer.setpointerbarrier"))
            continue;

          if (Name.startswith("llvm.lifetime.")) {
            // Remove lifetime intrinsics, these are an optimization only
          } else {
            LLVM_DEBUG(dbgs() << "Failed to handle call taking global address: "
                              << *Use << "\n");
            llvm_unreachable("Failed to handle call taking global address");
          }
          I->eraseFromParent();
        } else {
          LLVM_DEBUG(dbgs() << "Failed to handle call taking global address: "
                            << *Use << "\n");
          llvm_unreachable("Failed to handle call taking global address");
        }
      }
    }
  }

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
