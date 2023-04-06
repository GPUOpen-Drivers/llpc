/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  LowerDebugPrintf.cpp
 * @brief LLPC source file: contains implementation of class lgc::LowerDebugPrintf.
 ***********************************************************************************************************************
 */
#include "lgc/patch/LowerDebugPrintf.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lower-debug-printf"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerDebugPrintf::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-debug-printf\n");
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  Patch::init(&module);
  m_pipelineState = pipelineState;
  SmallVector<CallInst *> callees;
  BuilderBase builder(*m_context);
  for (auto &func : module) {
    auto name = func.getName();
    if (name.startswith(lgcName::LowerDebugPrintf)) {
      for (auto user : func.users()) {
        if (CallInst *callInst = dyn_cast<CallInst>(user)) {
          Value *resultVal = builder.getInt64(0);
          if (!isa<PoisonValue>(callInst->getArgOperand(0))) {
            builder.SetInsertPoint(callInst);
            resultVal = createDebugPrintf(callInst->getArgOperand(0), callInst->getArgOperand(1),
                                          make_range(callInst->arg_begin() + 2, callInst->arg_end()), builder);
          }
          callInst->replaceAllUsesWith(resultVal);
          callees.push_back(callInst);
        }
      }
    }
  }

  for (auto callInst : callees) {
    callInst->eraseFromParent();
  }

  if (m_elfInfos.empty())
    return PreservedAnalyses::all();

  setupElfsPrintfStrings();
  return PreservedAnalyses::allInSet<CFGAnalyses>();
}

// =====================================================================================================================
// Create debug printf operation, and write to the output debug buffer
// @debugPrintfBuffer : Output buffer for debug print data
// @formatStr : Printf format string
// @vars : Printf variable parameters
// @builder : BuilderBase to build instruction
Value *LowerDebugPrintf::createDebugPrintf(Value *debugPrintfBuffer, Value *formatStr,
                                           iterator_range<User::op_iterator> vars, BuilderBase &builder) {

  // Printf output variables in DWORDs

  SmallVector<Value *> printArgs;
  // Records printf output variables are 64bit or not
  SmallBitVector bit64Vector;
  for (const auto &var : vars) {
    getDwordValues(var, printArgs, bit64Vector, builder);
  }

  GlobalVariable *globalStr = cast<GlobalVariable>(formatStr);
  StringRef strDebugStr = (cast<ConstantDataSequential>(globalStr->getInitializer()))->getAsString();

  uint64_t hash = hash_value(strDebugStr);

  static const unsigned EntryHeaderSize = 2; // 2Dword EntrySize + stringId
  unsigned entrySize = EntryHeaderSize + printArgs.size();

  // 64 bit header {[0:15], [16:63]} entrySize,hash value for the string
  uint64_t header = (hash << 16) | entrySize;
  // save 48bit hash value to the m_formatStrings
  uint64_t hashValue = header >> 16;
  m_elfInfos[hashValue].formatString = strDebugStr;
  m_elfInfos[hashValue].bit64Pos = bit64Vector;

  uint32_t loEntryheader = uint32_t(header);
  uint32_t hiEntryheader = uint32_t(header >> 32);

  auto offsetTy = PointerType::get(builder.getInt64Ty(), ADDR_SPACE_BUFFER_FAT_POINTER);
  Value *bufferPtr = builder.CreateBitCast(debugPrintfBuffer, offsetTy);
  // uint64_t offset = AtomicAdd64(offsetPtr, entrySize);
  // maxOffset = 1<<31;
  // offset = offset < maxOffset ? offset : maxOffset;
  Value *entryOffset = builder.CreateAtomicRMW(AtomicRMWInst::Add, bufferPtr, builder.getInt64(entrySize), MaybeAlign(),
                                               AtomicOrdering::Monotonic, SyncScope::SingleThread);
  Value *maxOffset = builder.getInt64(1U << 31);
  entryOffset = builder.CreateBinaryIntrinsic(Intrinsic::umin, entryOffset, maxOffset);

  // Buffer Header is {BufferOffset_Loword, BufferOffset_Hiword, reserved0, reserved1};
  Type *bufferHeaderType = ArrayType::get(builder.getInt32Ty(), 4);
  Type *runtimeArrayType = ArrayType::get(builder.getInt32Ty(), ~0U);
  Type *bufferType = StructType::get(*m_context, {bufferHeaderType, runtimeArrayType});
  Type *bufferPtrType = PointerType::get(bufferType, ADDR_SPACE_BUFFER_FAT_POINTER);

  bufferPtr = builder.CreateBitCast(debugPrintfBuffer, bufferPtrType);

  SmallVector<Value *> outputVals = {builder.getInt32(loEntryheader), builder.getInt32(hiEntryheader)};
  outputVals.reserve(printArgs.size() + 2);
  // Prepare the dword sequence of printf output variables
  for (auto printArg : printArgs)
    outputVals.push_back(printArg);

  // Write the payload to debug buffer
  for (auto outValue : outputVals) {
    Value *bufferOutputPos =
        builder.CreateGEP(bufferType, bufferPtr, {builder.getInt32(0), builder.getInt32(1), entryOffset});
    builder.CreateStore(outValue, bufferOutputPos);
    entryOffset = builder.CreateAdd(entryOffset, builder.getInt64(1));
  }

  return entryOffset;
}

// =====================================================================================================================
// Convert value to the DWords, also append to output vector
// @val : input value
// @output : generated converted val
// @output64Bits : bits vector, one bit for one printf output variable
// @builder: builder to generate llvm
void LowerDebugPrintf::getDwordValues(Value *val, SmallVectorImpl<Value *> &output, SmallBitVector &output64Bits,
                                      BuilderBase &builder) {
  auto vTy = val->getType();

  auto int32Ty = builder.getInt32Ty();
  auto int64Ty = builder.getInt64Ty();

  switch (vTy->getTypeID()) {
  case Type::FixedVectorTyID: {
    for (uint64_t i = 0; i < cast<FixedVectorType>(vTy)->getNumElements(); ++i) {
      Value *element = builder.CreateExtractElement(val, i);
      return getDwordValues(element, output, output64Bits, builder);
    }
    break;
  }
  case Type::HalfTyID: {
    val = builder.CreateFPExt(val, builder.getFloatTy());
    return getDwordValues(val, output, output64Bits, builder);
  }
  case Type::FloatTyID: {
    val = builder.CreateBitCast(val, int32Ty);
    output.push_back(val);
    output64Bits.push_back(false);
    break;
  }
  case Type::DoubleTyID: {
    val = builder.CreateBitCast(val, int64Ty);
    return getDwordValues(val, output, output64Bits, builder);
  }

  case Type::IntegerTyID: {
    auto intTy = cast<IntegerType>(vTy);
    switch (intTy->getBitWidth()) {
    case 64: {
      Value *lowDword = builder.CreateTrunc(val, int32Ty);
      Value *highDword = builder.CreateLShr(val, 32);
      highDword = builder.CreateTrunc(highDword, int32Ty);
      output.push_back(lowDword);
      output.push_back(highDword);
      output64Bits.push_back(true);
      break;
    }
    case 32: {
      output.push_back(val);
      output64Bits.push_back(false);
      break;
    }
    case 1:
    case 8:
    case 16: {
      val = builder.CreateZExt(val, int32Ty);
      output.push_back(val);
      output64Bits.push_back(false);
      break;
    }
    default: {
      llvm_unreachable("Invalid integer type");
      break;
    }
    }
    break;
  }
  default: {
    llvm_unreachable("Invalid type");
    break;
  }
  }
}

void LowerDebugPrintf::setupElfsPrintfStrings() {

  msgpack::Document *document = m_pipelineState->getPalMetadata()->getDocument();
  auto printfStrings =
      document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::PrintfStrings].getMap(true);
  printfStrings[".version"] = 1;
  auto formatStrings = printfStrings[".strings"].getArray(true);
  unsigned i = 0;
  for (auto it = m_elfInfos.begin(); it != m_elfInfos.end(); ++it, ++i) {
    auto arrayElems = formatStrings[i].getMap(true);
    arrayElems[Util::Abi::PipelineMetadataKey::Index] = it->first;
    arrayElems[Util::Abi::PipelineMetadataKey::String] = it->second.formatString;
    auto &bitVector = it->second.bit64Pos;
    unsigned argsCount = bitVector.size();
    arrayElems[".argument_count"] = argsCount;
    // Convert bit array to the 64bits array
    unsigned bit64ArgsCount = (argsCount + 63) / 64;
    SmallVector<uint64_t> bitInDword64s(bit64ArgsCount, 0);
    for (unsigned j = 0; j < argsCount; ++j) {
      bitInDword64s[j / 64] |= (bitVector[j] << (j % 64));
    }
    auto bit64Args = arrayElems[".64bit_arguments"].getArray(true);
    for (unsigned j = 0; j < bit64ArgsCount; ++j)
      bit64Args[j] = bitInDword64s[j];
  }
}

} // namespace lgc
