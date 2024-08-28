//===- DxilToLlvm.cpp - Convert DXIL to LLVM IR. -===//
//
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
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "compilerutils/DxilToLlvm.h"
#include "compilerutils/TypeLowering.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace CompilerUtils;

#define DEBUG_TYPE "dxil-to-llvm"

namespace {

// Applies value replacements to values in metadata (ValueAsMetadata).
// Metadata values can be replaced in-place without the need to
// construct new objects. This simplifies the algorithm:
// We just traverse all reachable metadata nodes and update them on the fly.
// TODO: Can this be done more efficiently, e.g. by using ValueAsMetadata in LLVMContextImpl::ValuesAsMetadata?
//       This would need LLVM changes though to somehow expose that.
class MetadataUpdater {
public:
  MetadataUpdater(Module &module, TypeLowering &typeLower) : m_module{module}, m_typeLower{typeLower} {}

  void run() {
    processNamedMetadata();
    processUnnamedMetadata();
  }

private:
  void processNamedMetadata() {
    for (NamedMDNode &mdNode : m_module.named_metadata()) {
      for (auto node : mdNode.operands())
        processNode(node);
    }
  }

  void processUnnamedMetadata() {
    SmallVector<std::pair<unsigned, MDNode *>> collectedNodes;
    auto processCollectedNodes = [&]() {
      for (auto [_, mdNode] : collectedNodes)
        processNode(mdNode);
      collectedNodes.clear();
    };

    for (GlobalVariable &global : m_module.globals()) {
      global.getAllMetadata(collectedNodes);
      processCollectedNodes();
    }

    for (Function &func : m_module) {
      func.getAllMetadata(collectedNodes);
      processCollectedNodes();
      for (BasicBlock &bb : func) {
        for (Instruction &inst : bb) {
          inst.getAllMetadata(collectedNodes);
          processCollectedNodes();
        }
      }
    }
  }

  // Performs type and value replacements on the given ValueAsMetadata node
  void processValueMd(ValueAsMetadata *valueMd) {
    Value *oldValue = valueMd->getValue();

    auto types = m_typeLower.convertType(oldValue->getType());
    assert(types.size() == 1);
    if (types[0] == oldValue->getType())
      return;

    Value *newValue = m_typeLower.getValue(oldValue)[0];
    if (newValue != oldValue) {
      valueMd->handleRAUW(oldValue, newValue);
      LLVM_DEBUG(dbgs() << "Replaced " << *oldValue << " by " << newValue << "\n");
    } else {
      LLVM_DEBUG(dbgs() << "Kept value " << *oldValue << "\n");
    }
  }

  // For the given node and all reachable nodes (via operands):
  // Replace ValueAsMetadata values according to the stored type lowering object.
  // Ignores nodes that have already been processed.
  void processNode(MDNode *node) {
    // Adds a node to the worklist if it hasn't been seen before
    auto addToWorklist = [this](MDNode *node) {
      bool inserted = m_processed.insert(node).second;
      if (inserted)
        m_worklist.push_back(node);
    };

    addToWorklist(node);

    while (!m_worklist.empty()) {
      MDNode *curNode = m_worklist.pop_back_val();
      const auto *mdTuple = cast<MDTuple>(curNode);
      for (const MDOperand &operand : mdTuple->operands()) {
        Metadata *md = operand.get();
        if (!md)
          continue;
        if (auto *operandMdNode = dyn_cast<MDNode>(md))
          addToWorklist(operandMdNode);
        else if (auto *valueMd = dyn_cast<ValueAsMetadata>(md))
          processValueMd(valueMd);
        else
          assert(isa<MDString>(md));
      }
      continue;
    }
  }

  Module &m_module;
  TypeLowering &m_typeLower;
  // Bookkeeping data structures for pending and processed nodes.
  // These are only used in processNode()
  SmallVector<MDNode *> m_worklist;
  DenseSet<MDNode *> m_processed;
};

struct DxilToLlvmPassImpl {
  DxilToLlvmPassImpl(Module &module) : m_module(module), m_typeLower(module.getContext()) {}

  // Given a type used as element type of a vector, return the replacement type to be used in vectors.
  static Type *convertVectorElementType(Type *elemTy) {
    // For now, just replace i1 vectors as DXC is known to rely on the layout of i1 vectors
    // using pointer bitcasts.
    if (elemTy->isIntegerTy(1))
      return Type::getInt32Ty(elemTy->getContext());
    return nullptr;
  }

  static SmallVector<Type *> convertVectorType(TypeLowering &typeLower, Type *ty) {
    VectorType *vTy = dyn_cast<VectorType>(ty);
    if (!vTy)
      return {};
    assert(!vTy->isScalableTy());

    Type *elemTy = vTy->getElementType();
    Type *convertedElemTy = convertVectorElementType(elemTy);
    if (!convertedElemTy)
      return {};

    assert(convertedElemTy != elemTy);
    return {VectorType::get(convertedElemTy, vTy->getElementCount())};
  }

  // Wrapper around TypeLowering::convertType unpacking the vector.
  Type *getConvertedType(Type *ty) {
    auto types = m_typeLower.convertType(ty);
    assert(types.size() == 1);
    return types[0];
  }

  // Given a value of a converted type that has already been handled, obtain the replaced value.
  // Wrapper around TypeLowering::getValue unpacking the vector.
  Value *getConvertedValue(Value *value) {
    auto convertedValue = m_typeLower.getValue(value);
    assert(convertedValue.size() == 1);
    return convertedValue[0];
  }

  // Given an integer value that is replaced if part of a vector, create a value
  // of the replacement vector element type that can be used in the replaced
  // vector value.
  // The resulting value is a zext of the original value.
  Value *convertIntegerValue(IRBuilder<> &builder, Value *integerValue, Type *targetTy) {
    Type *origTy = integerValue->getType();
    assert(targetTy && targetTy != origTy);
    if ([[maybe_unused]] IntegerType *origIntegerTy = dyn_cast<IntegerType>(origTy)) {
      IntegerType *convertedIntegerTy = cast<IntegerType>(targetTy);
      assert(convertedIntegerTy->getBitWidth() >= origIntegerTy->getBitWidth());
      return builder.CreateZExt(integerValue, convertedIntegerTy);
    }
    llvm_unreachable("unsupported type");
  }

  // Given a converted integer value, restore a value of the original integer type.
  // Assumes the the original type bit width is smaller, and creates a trunc.
  Value *restoreIntegerValue(IRBuilder<> &builder, Value *convertedValue, Type *origTy) {
    if ([[maybe_unused]] IntegerType *origIntegerTy = dyn_cast<IntegerType>(origTy)) {
      assert(origIntegerTy->getBitWidth() <= convertedValue->getType()->getIntegerBitWidth());
      return builder.CreateTrunc(convertedValue, origTy);
    }
    llvm_unreachable("unsupported type");
  }

  // ; %vec is a value of a vector type that is replaced
  // ; %val is a value that is replaced within vectors
  // %vec.inserted = insertelement <2 x i1> %vec, i1 %val, i32 %idx
  //
  // --->
  //
  // %val.zext = zext i1 %val to i32
  // %vec.inserted.translated = insertelement <2 x i32> %vec.translated, i32 %val.zext, i32 %idx
  void visitInsertElement(llvm::InsertElementInst &insertElement) {
    Value *element = insertElement.getOperand(1);
    if (convertVectorElementType(element->getType()) == nullptr)
      return;

    IRBuilder<> builder(&insertElement);
    Value *inputVector = insertElement.getOperand(0);
    Value *index = insertElement.getOperand(2);

    auto convertedInputVector = getConvertedValue(inputVector);
    VectorType *convertedVectorTy = cast<VectorType>(convertedInputVector->getType());
    auto replacedElement = convertIntegerValue(builder, element, convertedVectorTy->getElementType());

    auto *replacement =
        builder.CreateInsertElement(convertedInputVector, replacedElement, index, insertElement.getName());
    m_typeLower.replaceInstruction(&insertElement, replacement);
  }

  // ; %vec is a value of a vector type that is replaced
  // %val = extractelement <2 x i1> %vec, i32 %idx
  //
  // --->
  //
  // %val.tmp = extractelement <2 x i32> %vec.translated, i32 %idx
  // %val.translated = trunc i32 %val.tmp to i1
  void visitExtractElement(llvm::ExtractElementInst &extractElement) {
    Value *inputVector = extractElement.getOperand(0);
    Type *elementTy = cast<VectorType>(inputVector->getType())->getElementType();
    if (convertVectorElementType(elementTy) == nullptr)
      return;

    Value *index = extractElement.getOperand(1);

    auto convertedInputVector = getConvertedValue(inputVector);

    IRBuilder<> builder(&extractElement);
    auto *convertedExtract = builder.CreateExtractElement(convertedInputVector, index, extractElement.getName());

    // Don't need to record any mapping, as the result type is a scalar which isn't replaced, so a RAUW is all we need.
    auto restoredElement = restoreIntegerValue(builder, convertedExtract, extractElement.getType());
    extractElement.replaceAllUsesWith(restoredElement);
    m_typeLower.eraseInstruction(&extractElement);
  }

  void visitGEP(llvm::GetElementPtrInst &gepInst) {
    Type *oldTy = gepInst.getSourceElementType();
    Type *newTy = getConvertedType(oldTy);
    // We intentionally only replace the GEP source type, and do not
    // update indices accordingly. In cases where the new type has
    // a different layout, this changes the GEP offset in the LLVM interpretation.
    // This is intended: The old and new GEPs have the same offset in the DXIL model,
    // which also equals the new GEPs offset in the LLVM model.
    if (newTy == oldTy)
      return;

    IRBuilder<> builder(&gepInst);

    // Type lowering may have changed pointer values, e.g. by creating a new alloca of matching type,
    // so we need to check replacement values.
    Value *pointerOperand = gepInst.getPointerOperand();
    auto convertedPointerOperand = m_typeLower.getValueOptional(pointerOperand);
    if (!convertedPointerOperand.empty())
      pointerOperand = convertedPointerOperand[0];

    SmallVector<Value *> indexList(gepInst.indices());
    auto *convertedGep = builder.CreateGEP(newTy, pointerOperand, indexList, gepInst.getName(), gepInst.isInBounds());

    gepInst.replaceAllUsesWith(convertedGep);
    m_typeLower.eraseInstruction(&gepInst);
  }

  void fixFunctionTypes() {
    for (Function &function : m_module)
      m_typeLower.lowerFunctionArguments(function);
  }

  llvm::PreservedAnalyses run() {
    m_typeLower.addRule(convertVectorType);

    static const auto visitor = llvm_dialects::VisitorBuilder<DxilToLlvmPassImpl>()
                                    .nest(&TypeLowering::registerVisitors)
                                    .add(&DxilToLlvmPassImpl::visitInsertElement)
                                    .add(&DxilToLlvmPassImpl::visitExtractElement)
                                    .add(&DxilToLlvmPassImpl::visitGEP)
                                    .build();
    fixFunctionTypes();

    visitor.visit(*this, m_module);

    m_typeLower.finishPhis();
    m_typeLower.finishCleanup();

    MetadataUpdater mdUpdater(m_module, m_typeLower);
    mdUpdater.run();

    return PreservedAnalyses::none();
  }
  Module &m_module;
  TypeLowering m_typeLower;
};

} // anonymous namespace

template <> struct llvm_dialects::VisitorPayloadProjection<DxilToLlvmPassImpl, TypeLowering> {
  static TypeLowering &project(DxilToLlvmPassImpl &p) { return p.m_typeLower; }
};

llvm::PreservedAnalyses CompilerUtils::DxilToLlvmPass::run(llvm::Module &module, llvm::ModuleAnalysisManager &) {
  DxilToLlvmPassImpl Impl{module};
  return Impl.run();
}
