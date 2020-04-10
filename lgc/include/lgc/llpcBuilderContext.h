/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderContext.h
 * @brief LLPC header file: declaration of llpc::BuilderContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"

namespace llvm {

class LLVMContext;
class ModulePass;
class raw_pwrite_stream;
class TargetMachine;
class Timer;

namespace legacy {

class PassManager;

} // namespace legacy

} // namespace llvm

namespace lgc {

class Builder;
class PassManager;
class Pipeline;
class TargetInfo;

// =====================================================================================================================
// BuilderContext class, used to create Pipeline and Builder objects. State shared between multiple compiles
// is kept here.
class BuilderContext {
public:
  // Initialize the middle-end. This must be called before the first BuilderContext::Create, although you are
  // allowed to call it again after that. It must also be called before LLVM command-line processing, so
  // that you can use a pass name in an option such as -print-after. If multiple concurrent compiles are
  // possible, this should be called in a thread-safe way.
  static void initialize();

  // Create the BuilderContext. Returns nullptr on failure to recognize the AMDGPU target whose name is specified
  //
  // @param context : LLVM context to use on all compiles
  // @param gpuName : LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
  // @param palAbiVersion : PAL pipeline ABI version to compile for
  static BuilderContext *Create(llvm::LLVMContext &context, llvm::StringRef gpuName, unsigned palAbiVersion);

  ~BuilderContext();

  // Get LLVM context
  llvm::LLVMContext &getContext() const { return m_context; }

  // Get the target machine.
  llvm::TargetMachine *getTargetMachine() const { return m_targetMachine; }

  // Get targetinfo
  const TargetInfo &getTargetInfo() const { return *m_targetInfo; }

  // Get the PAL pipeline ABI version to compile for
  unsigned getPalAbiVersion() const { return m_palAbiVersion; }

  // Create a Pipeline object for a pipeline compile
  Pipeline *createPipeline();

  // Create a Builder object. For a shader compile (pPipelineState is nullptr), useBuilderRecorder is ignored
  // because it always uses BuilderRecorder.
  //
  // @param pipeline : Pipeline object for pipeline compile, nullptr for shader compile
  // @param useBuilderRecorder : True to use BuilderRecorder, false to use BuilderImpl
  Builder *createBuilder(Pipeline *pipeline, bool useBuilderRecorder);

  // Prepare a pass manager. This manually adds a target-aware TLI pass, so middle-end optimizations do not
  // think that we have library functions.
  //
  // @param [in/out] passMgr : Pass manager
  void preparePassManager(llvm::legacy::PassManager *passMgr);

  // Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
  void addTargetPasses(lgc::PassManager &passMgr, llvm::Timer *codeGenTimer, llvm::raw_pwrite_stream &outStream);

  void setBuildRelocatableElf(bool buildRelocatableElf) { m_buildRelocatableElf = buildRelocatableElf; }
  bool buildingRelocatableElf() { return m_buildRelocatableElf; }

  // Utility method to create a start/stop timer pass
  static llvm::ModulePass *createStartStopTimer(llvm::Timer *timer, bool starting);

  // Set and get a pointer to the stream used for LLPC_OUTS. This is initially nullptr,
  // signifying no output from LLPC_OUTS. Setting this to a stream means that LLPC_OUTS
  // statements in the middle-end output to that stream, giving a dump of LLVM IR at a
  // few strategic places in the pass flow, as well as information such as input/output
  // mapping.
  static void setLlpcOuts(llvm::raw_ostream *stream) { m_llpcOuts = stream; }
  static llvm::raw_ostream *getLgcOuts() { return m_llpcOuts; }

private:
  BuilderContext() = delete;
  BuilderContext(const BuilderContext &) = delete;
  BuilderContext &operator=(const BuilderContext &) = delete;

  BuilderContext(llvm::LLVMContext &context, unsigned palAbiVersion);

  // -----------------------------------------------------------------------------------------------------------------
  static llvm::raw_ostream *m_llpcOuts;           // nullptr or stream for LLPC_OUTS
  llvm::LLVMContext &m_context;                   // LLVM context
  llvm::TargetMachine *m_targetMachine = nullptr; // Target machine
  TargetInfo *m_targetInfo = nullptr;             // Target info
  bool m_buildRelocatableElf = false;             // Flag indicating whether we are building relocatable ELF
  unsigned m_palAbiVersion = 0xFFFFFFFF;          // PAL pipeline ABI version to compile for
};

} // namespace lgc
