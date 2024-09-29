/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  LowerExecutionGraph.h
 * @brief LLPC header file: contains declaration of Llpc::LowerExecutionGraph
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "SPIRVInternal.h"
#include "lgc/LgcWgDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/PassManager.h"

namespace CompilerUtils {
class TypeLowering;
} // namespace CompilerUtils

namespace lgc {
class Pipeline;
struct ComputeShaderMode;
} // namespace lgc

namespace Llpc {

namespace WorkCreationScope {
enum : unsigned {
  Invocation = 0, // WorkCreation library invocation scope
  Workgroup = 1,  // WorkCreation library workgroup scope
  Subgroup = 2    // WorkCreation library subgroup scope
};
}

namespace WorkGraphBuiltIns {
enum : unsigned {
  CoalescedInputCount = 0,  // SPIRV CoalescedInputCount
  WorkgroupId,              // SPIRV WorkgroupId
  GlobalInvocationId,       // SPIRV GlobalInvocationId
  ShaderIndex,              // SPIRV ShaderIndex
  RemainingRecursionLevels, // SPIRV RemainingRecursionLevels
  LocalInvocationIndex,     // SPIRV GlobalInvocationId
  Count
};
}

namespace OutputAllocateArg {
enum : unsigned { ShaderState = 0, Scope, OutputIdx, ArrayIdx, Count };
}

// =====================================================================================================================
// Represents the pass of SPIR-V lowering shader enqueue opcode
class LowerExecutionGraph : public SpirvLower, public llvm::PassInfoMixin<LowerExecutionGraph> {

  struct OutputPayloadInfo {
    unsigned payloadCount;    // Payload Count
    unsigned payloadSize;     // Payload Size
    unsigned payloadId;       // Payload id
    unsigned limitSharedWith; // payload id to share with limit
    unsigned scope;           // created scope
    bool trackFinishWriting;  // Whether this payload need to track finish writing
    unsigned arraySize;       // Payload array size
    unsigned arrayTypeId;     // Payload array type's id
    unsigned dynamicDispatch; // DynamicDispatch;
  };

  struct InputPayloadInfo {
    llvm::StringRef nodeName;   // node name
    unsigned arrayIndex;        // array Index
    unsigned payloadCount;      // Payload Count
    unsigned payloadSize;       // Payload Size
    bool trackFinishWriting;    // Track finish
    unsigned dynamicDispatch;   // DynamicDispatch
    unsigned nodeType;          // Node type
    unsigned vbTableOffset;     // vertex buffer table offset
    unsigned indexBufferOffset; // index buffer table offset
  };

public:
  LowerExecutionGraph(lgc::Pipeline *pipeline);
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower SPIR-V execution graph node shader"; }

private:
  void initAllocVariables(lgc::Builder *builder);
  typedef void (LowerExecutionGraph::*LibraryFuncPtr)(llvm::Function *, unsigned);
  llvm::Type *getShaderStateTy();
  llvm::Type *getOutputRecordsTy();
  void getFuncRets(llvm::Function *func, llvm::SmallVector<llvm::Instruction *, 4> &rets);
  void lowerGlobals(unsigned enqueueMetaId, unsigned inoutMetaId);
  void processBuiltinGlobals(llvm::GlobalVariable *global, llvm::MDNode *mdata);
  void buildExecGraphNodeMetadata(const lgc::wg::ShaderEnqueueMode &enqueueModes, const InputPayloadInfo &payloads);
  void initInputPayloadInfo(const lgc::wg::ShaderEnqueueMode &enqueueModes);
  llvm::GlobalVariable *getInputPayload(unsigned enqueueMetaId);
  void createGraphLds(unsigned outputCount);
  unsigned getOutputIndex(unsigned payloadId);
  void visitIndexPayloadArray(lgc::wg::IndexPayloadArrayOp &inst);
  void visitAllocateNodePayloads(lgc::wg::AllocateNodePayloadsOp &inst);
  void visitRegisterOutputNode(lgc::wg::RegisterOutputNodeOp &inst);
  void visitEnqueueNodePayloads(lgc::wg::EnqueueNodePayloadsOp &inst);
  void visitPayloadArrayLength(lgc::wg::PayloadArrayLengthOp &inst);
  void visitIsNodePayloadValid(lgc::wg::IsNodePayloadValidOp &inst);
  void visitFinishWritingNodePayload(lgc::wg::FinishWritingNodePayloadOp &inst);
  void visitGraphGetLds(lgc::wg::GraphGetLdsOp &inst);
  void visitOutputCount(lgc::wg::OutputCountOp &inst);
  llvm_dialects::VisitorResult visitLoad(LoadInst &load);
  llvm_dialects::VisitorResult visitAlloca(AllocaInst &alloca);
  llvm_dialects::VisitorResult visitStore(StoreInst &store);
  llvm_dialects::VisitorResult visitGetElementPtr(GetElementPtrInst &gep);
  Type *replacePayloadType(Type *ty);
  bool isThreadLaunchNode(const lgc::ComputeShaderMode &shaderMode, const lgc::wg::ShaderEnqueueMode &enqueueModes,
                          const InputPayloadInfo &payloads);
  std::array<llvm::Value *, 5> m_outputAllocateArgs;
  llvm::Value *m_tempVariable;
  llvm::GlobalVariable *m_localInvocationIndex;                       // Built-in variable
  llvm::GlobalVariable *m_builtInVariables[WorkGraphBuiltIns::Count]; // Built-in variable
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;                 // Function to lower
  llvm::MapVector<llvm::StringRef, OutputPayloadInfo> m_nodeNamesIdx; // Node names
  llvm::DenseMap<llvm::StringRef, unsigned> m_workGraphLibFuncNames;  // Workgraph library functions names
  llvm::SmallVector<llvm::Function *> m_graphLibFuncs;                // Workgraph library
  llvm::Type *m_payloadArrayPtrType = nullptr;
  CompilerUtils::TypeLowering *m_typeLowering = nullptr;
  lgc::wg::ShaderEnqueueMode m_enqueueModes;
  std::string m_inputSharedWithName;

  unsigned m_metaEnqueueId;            // Shader enqueue meta id
  lgc::Pipeline *m_pipeline;           // Pipeline State
  InputPayloadInfo m_inputPayloadInfo; // Input payload info
  llvm::GlobalVariable *m_graphLds;    // Graph Lds variable
  bool m_threadLaunch;                 // Enable ThreadLaunch mode or not
};
} // namespace Llpc
