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
 * @file  LgcContext.h
 * @brief LLPC header file: declaration of llpc::LgcContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DerivedTypes.h"
#include <map>

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

// TODO: Taken from vkgcDefs.h.  If I include that file vulkan.h cannot be found.  It is also defined in
// ResourceUsage.h, which also cannot be inluded here.
/// Represents the base data type
enum class BasicVertexInputType : unsigned {
  Unknown = 0, ///< Unknown
  Float,       ///< Float
  Double,      ///< Double
  Int,         ///< Signed integer
  Uint,        ///< Unsigned integer
  Int64,       ///< 64-bit signed integer
  Uint64,      ///< 64-bit unsigned integer
  Float16,     ///< 16-bit floating-point
  Int16,       ///< 16-bit signed integer
  Uint16,      ///< 16-bit unsigned integer
  Int8,        ///< 8-bit signed integer
  Uint8,       ///< 8-bit unsigned integer
};

// Struct used to represent the type of a vertext input.
struct VertexInputTypeInfo {
  unsigned vectorSize;
  BasicVertexInputType elementType;
  uint32_t argumentIndex;

  void setArgumentIndex(unsigned long idx);
};

// Information about the vertex shader used to generate the fetch shader.
// TODO: This should moved to a new file.  Where should that file live?  It should also be part of the generic
// interface data, but putting it there will require some changes that do not play well with caching.  I propose
// we open and issue that is block by the refactoring of the linking code.  That issue will address the caching of fetch
// shaders and hiding this class inside lgc.
class VsInterfaceData {
public:
  typedef std::map<std::pair<unsigned, unsigned>, VertexInputTypeInfo> LocationToTypeMap;

  // Record the type of the vertex input at the given location.
  unsigned int setVertexInputType(unsigned location, unsigned component, llvm::Type *type);
  void setVertexInputType(unsigned location, unsigned component, const VertexInputTypeInfo &typeInfo) {
    m_vertexInputType[{location, component}] = typeInfo;
  }

  // Get the type of the vertex input at the given location.
  llvm::Type *getVertexInputType(unsigned location, unsigned component, llvm::LLVMContext *context) const;
  const LocationToTypeMap &getVertexInputTypeInfo() const { return m_vertexInputType; }
  LocationToTypeMap &getVertexInputTypeInfo() { return m_vertexInputType; }

  // Clears the types that have been recored for the vertex inputs.
  void clearVertexInputTypeInfo() { m_vertexInputType.clear(); }

  // Get and set the number of the last SGPR that is used as an input to the vertex shader.
  unsigned getLastSgpr() const { return m_lastSgpr; }
  void setLastSgpr(unsigned i) { m_lastSgpr = i; }

  // Get and set the number of the SGPR in which the vertex shader is expecting the vertex offset.
  unsigned getBaseVertexRegister() const { return m_baseVertexRegister; }
  void setBaseVertexRegister(unsigned i) { m_baseVertexRegister = i; }

  // Get and set the number of the SGPR in which the vertex shader is expecting the instance offset.
  unsigned getBaseInstanceRegister() const { return m_baseInstanceRegister; }
  void setBaseInstanceRegister(unsigned i) { m_baseInstanceRegister = i; }

  // Get and set the value of VGPR_COMP_CNT entry for the vertex shader.
  unsigned getVgprCompCnt() const { return m_vgrpCompCnt; }
  void setVgprCompCnt(unsigned int i) { m_vgrpCompCnt = i; }

  // Get and set the number of the SGPR in which the vertex shader is expecting the address of the vertex buffer table.
  unsigned getVertexBufferRegister() const { return m_vertexBufferRegister; }
  void setVertexBuffer(unsigned i) { m_vertexBufferRegister = i; }

private:
  unsigned m_lastSgpr;                 // The last SGPR used as an input to the vertext shader.
  unsigned m_baseVertexRegister;       // The SGPR that contains the vertex offset.
  unsigned m_baseInstanceRegister;     // The SGRP that contains the instance offset.
  unsigned m_vgrpCompCnt;              // The value of the VGRP_COMP_CNT in the vertex shader.
  unsigned m_vertexBufferRegister;     // The SGRP that contains the address of the vertex buffer table.
  LocationToTypeMap m_vertexInputType; // A map from a location to the type of the vertex input at that location.
};

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

  // Create the LgcContext. Returns nullptr on failure to recognize the AMDGPU target whose name is specified
  //
  // @param context : LLVM context to use on all compiles
  // @param gpuName : LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
  // @param palAbiVersion : PAL pipeline ABI version to compile for
  static LgcContext *Create(llvm::LLVMContext &context, llvm::StringRef gpuName, unsigned palAbiVersion);

  ~LgcContext();

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

  // Tell the context whether or not a fetch shader is to be built.
  void setBuildFetchShader(bool buildFetchShader) { m_buildFetchShader = buildFetchShader; }

  // Returns true if a fetch shader should be built.
  bool buildingFetchShader() { return m_buildFetchShader; }

  // Utility method to create a start/stop timer pass
  static llvm::ModulePass *createStartStopTimer(llvm::Timer *timer, bool starting);

  // Set and get a pointer to the stream used for LLPC_OUTS. This is initially nullptr,
  // signifying no output from LLPC_OUTS. Setting this to a stream means that LLPC_OUTS
  // statements in the middle-end output to that stream, giving a dump of LLVM IR at a
  // few strategic places in the pass flow, as well as information such as input/output
  // mapping.
  static void setLlpcOuts(llvm::raw_ostream *stream) { m_llpcOuts = stream; }
  static llvm::raw_ostream *getLgcOuts() { return m_llpcOuts; }

  VsInterfaceData *getVsInterfaceData() { return &m_vsInterfaceData; }
  const VsInterfaceData *getVsInterfaceData() const { return &m_vsInterfaceData; }

private:
  LgcContext() = delete;
  LgcContext(const LgcContext &) = delete;
  LgcContext &operator=(const LgcContext &) = delete;

  LgcContext(llvm::LLVMContext &context, unsigned palAbiVersion);

  // -----------------------------------------------------------------------------------------------------------------
  static llvm::raw_ostream *m_llpcOuts;           // nullptr or stream for LLPC_OUTS
  llvm::LLVMContext &m_context;                   // LLVM context
  llvm::TargetMachine *m_targetMachine = nullptr; // Target machine
  TargetInfo *m_targetInfo = nullptr;             // Target info
  unsigned m_palAbiVersion = 0xFFFFFFFF;          // PAL pipeline ABI version to compile for
  bool m_buildFetchShader = false;                // Flag indicating whether we are to generate a fetch shader

  // TODO: These should be moved to the interface data if possible.
  VsInterfaceData m_vsInterfaceData;
};

} // namespace lgc
