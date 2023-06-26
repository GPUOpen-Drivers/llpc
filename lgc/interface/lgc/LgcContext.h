/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LgcContext.h
 * @brief LLPC header file: declaration of llpc::LgcContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class LLVMContext;
class ModulePass;
class raw_pwrite_stream;
class TargetMachine;
class Timer;

} // namespace llvm

namespace llvm_dialects {

class DialectContext;

} // namespace llvm_dialects

namespace lgc {

class Builder;
class LegacyPassManager;
class PassManager;
class PassManagerCache;
class Pipeline;
class TargetInfo;

// Size in bytes of resource (image) descriptor
static constexpr unsigned DescriptorSizeResource = 8 * sizeof(uint32_t);
// Size in dwords of sampler descriptor
static constexpr unsigned DescriptorSizeSamplerInDwords = 4;
// Size in bytes of sampler descriptor
static constexpr unsigned DescriptorSizeSampler = DescriptorSizeSamplerInDwords * sizeof(uint32_t);
// Size in bytes of buffer descriptor
static constexpr unsigned DescriptorSizeBuffer = 4 * sizeof(uint32_t);

// =====================================================================================================================
// LgcContext class, used to create Pipeline and Builder objects. State shared between multiple compiles
// is kept here.
class LgcContext {
public:
  // Initialize the middle-end. This must be called before the first LgcContext::Create, although you are
  // allowed to call it again after that. It must also be called before LLVM command-line processing, so
  // that you can use a pass name in an option such as -print-after. If multiple concurrent compiles are
  // possible, this should be called in a thread-safe way.
  static void initialize();

  // Create TargetMachine. Returns nullptr on failure to recognize the AMDGPU target whose name is specified.
  //
  // @param gpuName : LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
  // @param optLevel : LLVM optimization level used to initialize target machine
  static std::unique_ptr<llvm::TargetMachine> createTargetMachine(llvm::StringRef gpuName,
                                                                  llvm::CodeGenOpt::Level optLevel);

  // Create the LgcContext.
  //
  // @param targetMachine : LLVM TargetMachine to use. Caller retains ownership and must free it when finished.
  // @param context : LLVM context to give each Builder. Caller retains ownership and must free it when finished.
  // @param palAbiVersion : PAL pipeline ABI version to compile for
  static LgcContext *create(llvm::TargetMachine *targetMachine, llvm::LLVMContext &context, unsigned palAbiVersion);

  // Get the value of the -emit-lgc option. BuilderRecorder uses this to decide whether to omit the opcode
  // metadata when recording a Builder call.
  static bool getEmitLgc();

  ~LgcContext();

  // Given major.minor.steppings - generate the gpuName string
  static std::string getGpuNameString(unsigned major, unsigned minor, unsigned stepping);

  // Verify that gpuName is valid
  static bool isGpuNameValid(llvm::StringRef gpuName);

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

  // Create a Builder object. This is now unnecessary, as you can just create a local variable Builder or "new" it
  // yourself.
  //
  // @param pipeline : Ignored
  Builder *createBuilder(Pipeline *pipeline);

  // Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
  void addTargetPasses(lgc::LegacyPassManager &passMgr, llvm::Timer *codeGenTimer, llvm::raw_pwrite_stream &outStream);

  // Returns the optimization level for the context.
  llvm::CodeGenOpt::Level getOptimizationLevel() const;

  // Returns the optimization level used for context initialization.
  llvm::CodeGenOpt::Level getInitialOptimizationLevel() const { return m_initialOptLevel; }

  // Utility method to create a start/stop timer pass
  static llvm::ModulePass *createStartStopTimer(llvm::Timer *timer, bool starting);

  // Utility method to create a start/stop timer pass and add it to the given
  // pass manager
  static void createAndAddStartStopTimer(lgc::PassManager &passMgr, llvm::Timer *timer, bool starting);

  // Set and get a pointer to the stream used for LLPC_OUTS. This is initially nullptr,
  // signifying no output from LLPC_OUTS. Setting this to a stream means that LLPC_OUTS
  // statements in the middle-end output to that stream, giving a dump of LLVM IR at a
  // few strategic places in the pass flow, as well as information such as input/output
  // mapping.
  static void setLlpcOuts(llvm::raw_ostream *stream) { m_llpcOuts = stream; }
  static llvm::raw_ostream *getLgcOuts() { return m_llpcOuts; }

  // Get pass manager cache
  PassManagerCache *getPassManagerCache();

private:
  LgcContext() = delete;
  LgcContext(const LgcContext &) = delete;
  LgcContext &operator=(const LgcContext &) = delete;

  LgcContext(llvm::LLVMContext &context, unsigned palAbiVersion);

  static llvm::raw_ostream *m_llpcOuts;           // nullptr or stream for LLPC_OUTS
  llvm::LLVMContext &m_context;                   // LLVM context
  llvm::TargetMachine *m_targetMachine = nullptr; // Target machine
  TargetInfo *m_targetInfo = nullptr;             // Target info
  unsigned m_palAbiVersion = 0xFFFFFFFF;          // PAL pipeline ABI version to compile for
  PassManagerCache *m_passManagerCache = nullptr; // Pass manager cache and creator
  llvm::CodeGenOpt::Level m_initialOptLevel;      // Optimization level at initialization
};

} // namespace lgc
