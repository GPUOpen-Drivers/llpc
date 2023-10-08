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

#include "lgc/util/CpsStackLowering.h"
#include "lgc/util/BuilderBase.h"
#include "llvm-dialects/Dialect/Visitor.h"

using namespace llvm;
using namespace lgc;

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(CpsStackLowering, m_typeLowering)
// =====================================================================================================================
// Type lowering rule that lowers cps stack pointer type to corresponding backend pointer type.
//
// @param typeLowering : the calling TypeLowering object
// @param type : the type to be converted
static SmallVector<Type *> convertCpsStackPointer(TypeLowering &typeLowering, Type *type) {
  SmallVector<Type *> types;

  if (auto *pointerType = dyn_cast<PointerType>(type)) {
    if (pointerType->getAddressSpace() == cps::stackAddrSpace)
      types.push_back(PointerType::get(type->getContext(), getLoweredCpsStackAddrSpace()));
  }

  return types;
}

// =====================================================================================================================
// Lower continuation stack operations in the function
//
// @param function : the function to be processed
// @param cpsStorage : the alloca used for the holding the latest continuation stack pointer
void CpsStackLowering::lowerCpsStackOps(Function &function, Value *cpsStorage) {
  m_module = function.getParent();
  m_cpsStackAlloca = cpsStorage;
  m_typeLowering.addRule(&convertCpsStackPointer);
  auto *newFunc = &function;
  if (cps::isCpsFunction(function))
    newFunc = m_typeLowering.lowerFunctionArguments(function);

  static const auto visitor = llvm_dialects::VisitorBuilder<CpsStackLowering>()
                                  .nest(&TypeLowering::registerVisitors)
                                  .add(&CpsStackLowering::visitCpsAlloc)
                                  .add(&CpsStackLowering::visitCpsFree)
                                  .add(&CpsStackLowering::visitCpsPeek)
                                  .add(&CpsStackLowering::visitSetVsp)
                                  .add(&CpsStackLowering::visitGetVsp)
                                  .add(&CpsStackLowering::visitGetElementPtr)
                                  .add(&CpsStackLowering::visitPtrToIntInst)
                                  .add(&CpsStackLowering::visitIntToPtrInst)
                                  .add(&CpsStackLowering::visitLoad)
                                  .add(&CpsStackLowering::visitStore)
                                  .build();
  visitor.visit(*this, *newFunc);
  m_typeLowering.finishPhis();
  m_typeLowering.finishCleanup();
}

// =====================================================================================================================
// Lower getelementptr instruction
//
// @param function : the instruction
void CpsStackLowering::visitGetElementPtr(GetElementPtrInst &getElemPtrInst) {
  if (getElemPtrInst.getAddressSpace() != cps::stackAddrSpace)
    return;

  auto values = m_typeLowering.getValue(getElemPtrInst.getPointerOperand());
  IRBuilder<> builder(&getElemPtrInst);

  SmallVector<Value *, 8> indices(getElemPtrInst.idx_begin(), getElemPtrInst.idx_end());

  Value *newGetElemPtr = nullptr;
  auto getElemPtrPtr = values[0];
  auto getElemPtrEltTy = getElemPtrInst.getSourceElementType();

  if (getElemPtrInst.isInBounds())
    newGetElemPtr = builder.CreateInBoundsGEP(getElemPtrEltTy, getElemPtrPtr, indices);
  else
    newGetElemPtr = builder.CreateGEP(getElemPtrEltTy, getElemPtrPtr, indices);

  cast<Instruction>(newGetElemPtr)->copyMetadata(getElemPtrInst);

  m_typeLowering.replaceInstruction(&getElemPtrInst, {newGetElemPtr});
}

// =====================================================================================================================
// Lower load instruction
//
// @param function : the instruction
void CpsStackLowering::visitLoad(LoadInst &load) {
  if (load.getPointerAddressSpace() != cps::stackAddrSpace)
    return;

  auto values = m_typeLowering.getValue(load.getPointerOperand());
  load.replaceUsesOfWith(load.getPointerOperand(), values[0]);
}

// =====================================================================================================================
// Lower store instruction
//
// @param function : the instruction
void CpsStackLowering::visitStore(llvm::StoreInst &store) {
  if (store.getPointerAddressSpace() != cps::stackAddrSpace)
    return;

  auto values = m_typeLowering.getValue(store.getPointerOperand());
  store.replaceUsesOfWith(store.getPointerOperand(), values[0]);
}

// =====================================================================================================================
// Lower ptrtoint instruction
//
// @param function : the instruction
void CpsStackLowering::visitPtrToIntInst(llvm::PtrToIntInst &ptr2Int) {
  if (ptr2Int.getPointerAddressSpace() != cps::stackAddrSpace)
    return;

  auto values = m_typeLowering.getValue(ptr2Int.getOperand(0));
  ptr2Int.replaceUsesOfWith(ptr2Int.getOperand(0), values[0]);
}

// =====================================================================================================================
// Lower inttoptr instruction
//
// @param function : the instruction
void CpsStackLowering::visitIntToPtrInst(llvm::IntToPtrInst &int2Ptr) {
  if (int2Ptr.getAddressSpace() != cps::stackAddrSpace)
    return;

  IRBuilder<> builder(&int2Ptr);
  auto *newPtr = builder.CreateIntToPtr(int2Ptr.getOperand(0),
                                        PointerType::get(builder.getContext(), getLoweredCpsStackAddrSpace()));
  m_typeLowering.replaceInstruction(&int2Ptr, newPtr);
}

// =====================================================================================================================
// Lower lgc.cps.alloc instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsAlloc(cps::AllocOp &alloc) {
  IRBuilder<> builder(&alloc);

  Value *size = alloc.getSize();
  const DataLayout &layout = m_module->getDataLayout();
  Value *vsp = builder.CreateAlignedLoad(builder.getPtrTy(getLoweredCpsStackAddrSpace()), m_cpsStackAlloca,
                                         Align(getLoweredCpsStackPointerSize(layout)));
  unsigned alignedSize = alignTo(cast<ConstantInt>(size)->getZExtValue(), continuationStackAlignment);
  m_stackSizeInBytes += alignedSize;

  // update stack pointer
  Value *ptr = builder.CreateConstGEP1_32(builder.getInt8Ty(), vsp, alignedSize);
  builder.CreateAlignedStore(ptr, m_cpsStackAlloca, Align(getLoweredCpsStackPointerSize(layout)));

  m_typeLowering.replaceInstruction(&alloc, {vsp});
}

// =====================================================================================================================
// Lower lgc.cps.free instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsFree(cps::FreeOp &freeOp) {
  IRBuilder<> builder(&freeOp);
  const DataLayout &layout = m_module->getDataLayout();

  Value *vsp = builder.CreateAlignedLoad(builder.getPtrTy(getLoweredCpsStackAddrSpace()), m_cpsStackAlloca,
                                         Align(getLoweredCpsStackPointerSize(layout)));
  Value *size = freeOp.getSize();
  unsigned alignedSize = alignTo(cast<ConstantInt>(size)->getZExtValue(), continuationStackAlignment);
  Value *ptr = builder.CreateConstGEP1_32(builder.getInt8Ty(), vsp, -alignedSize);
  // Assuming continuation stack grows upward.
  builder.CreateAlignedStore(ptr, m_cpsStackAlloca, Align(getLoweredCpsStackPointerSize(layout)));
  m_typeLowering.replaceInstruction(&freeOp, {});
}

// =====================================================================================================================
// Lower lgc.cps.peek instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsPeek(cps::PeekOp &peekOp) {
  IRBuilder<> builder(&peekOp);
  const DataLayout &layout = m_module->getDataLayout();

  auto *ptr = builder.CreateAlignedLoad(builder.getPtrTy(getLoweredCpsStackAddrSpace()), m_cpsStackAlloca,
                                        Align(getLoweredCpsStackPointerSize(layout)));
  auto *size = peekOp.getSize();
  unsigned immSize = cast<ConstantInt>(size)->getZExtValue();
  immSize = alignTo(immSize, continuationStackAlignment);
  // Assuming continuation stack grows upward.
  auto *result = builder.CreateGEP(builder.getInt8Ty(), ptr, {builder.getInt32(-immSize)});
  m_typeLowering.replaceInstruction(&peekOp, {result});
}

// =====================================================================================================================
// Lower lgc.cps.set.vsp instruction
//
// @param function : the instruction
void CpsStackLowering::visitSetVsp(cps::SetVspOp &setVsp) {
  IRBuilder<> builder(&setVsp);
  const DataLayout &layout = m_module->getDataLayout();

  auto *ptr = setVsp.getPtr();
  auto converted = m_typeLowering.getValue(ptr);
  builder.CreateAlignedStore(converted[0], m_cpsStackAlloca, Align(getLoweredCpsStackPointerSize(layout)));
  m_typeLowering.replaceInstruction(&setVsp, {});
}

// =====================================================================================================================
// Lower lgc.cps.get.vsp instruction
//
// @param function : the instruction
void CpsStackLowering::visitGetVsp(cps::GetVspOp &getVsp) {
  IRBuilder<> builder(&getVsp);
  const DataLayout &layout = m_module->getDataLayout();

  auto *ptr = builder.CreateAlignedLoad(builder.getPtrTy(getLoweredCpsStackAddrSpace()), m_cpsStackAlloca,
                                        Align(getLoweredCpsStackPointerSize(layout)));
  m_typeLowering.replaceInstruction(&getVsp, {ptr});
}
