/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PalMetadata.h
 * @brief LLPC header file: PalMetadata class for manipulating PAL metadata
 *
 * The PalMetadata object can be retrieved using PipelineState::getPalMetadata(), and is used by various parts
 * of LGC to write information to PAL metadata at the time the information is generated. The PalMetadata object
 * is carried through the middle-end, and serialized to IR metadata at the end of the middle-end (or at the
 * point -stop-before etc stops compilation, if earlier).
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "lgc/state/AbiMetadata.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lgc {

class PipelineState;

// =====================================================================================================================
// Class for manipulating PAL metadata through LGC
class PalMetadata {
public:
  // Constructors
  PalMetadata(PipelineState *pipelineState);
  PalMetadata(PipelineState *pipelineState, llvm::StringRef blob);
  PalMetadata(PipelineState *pipelineState, llvm::Module *module);
  PalMetadata(const PalMetadata &) = delete;
  PalMetadata &operator=(const PalMetadata &) = delete;

  ~PalMetadata();

  // Read blob as PAL metadata and merge it into existing PAL metadata (if any).
  void mergeFromBlob(llvm::StringRef blob);

  // Record the PAL metadata into IR metadata in the specified module.
  void record(llvm::Module *module);

  // Get the MsgPack document for explicit manipulation. Only ConfigBuilder* uses this.
  llvm::msgpack::Document *getDocument() { return m_document; }

  // Set the PAL metadata SPI register for one user data entry
  void setUserDataEntry(ShaderStage stage, unsigned userDataIndex, unsigned userDataValue, unsigned dwordCount = 1);
  void setUserDataEntry(ShaderStage stage, unsigned userDataIndex, UserDataMapping userDataValue,
                        unsigned dwordCount = 1) {
    setUserDataEntry(stage, userDataIndex, static_cast<unsigned>(userDataValue), dwordCount);
  }

  // Mark that the user data spill table is used at the given offset. The SpillThreshold PAL metadata entry is
  // set to the minimum of any call to this function in any shader.
  void setUserDataSpillUsage(unsigned dwordOffset);

  // Fix up user data registers. Any user data register that has one of the unlinked UserDataMapping values defined
  // in AbiUnlinked.h is fixed up by looking at pipeline state.
  void fixUpRegisters();

  // Set a register value in PAL metadata. If the register is already set, this ORs in the value.
  void setRegister(unsigned regNum, unsigned value);

  // Finalize PAL metadata for pipeline.
  // TODO Shader compilation: The idea is that this will be called at the end of a pipeline compilation, or in
  // an ELF link, but not at the end of a shader/half-pipeline compile.
  void finalizePipeline();

private:
  // Initialize the PalMetadata object after reading in already-existing PAL metadata if any
  void initialize();

  // Get the first user data register number for the given shader stage.
  unsigned getUserDataReg0(ShaderStage stage);

  // Set userDataLimit to maximum
  void setUserDataLimit();

  PipelineState *m_pipelineState;           // PipelineState
  llvm::msgpack::Document *m_document;      // The MsgPack document
  llvm::msgpack::MapDocNode m_pipelineNode; // MsgPack map node for amdpal.pipelines[0]
  llvm::msgpack::MapDocNode m_registers;    // MsgPack map node for amdpal.pipelines[0].registers
  // Mapping from ShaderStage to SPI user data register start, allowing for merged shaders and NGG.
  unsigned m_userDataRegMapping[ShaderStageCountInternal] = {};
  llvm::msgpack::DocNode *m_userDataLimit;  // Maximum so far number of user data dwords used
  llvm::msgpack::DocNode *m_spillThreshold; // Minimum so far dword offset used in user data spill table
};

} // namespace lgc
