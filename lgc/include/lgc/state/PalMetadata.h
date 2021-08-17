/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/Pipeline.h"
#include "lgc/state/AbiMetadata.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include <map>

namespace llvm {
class Module;
class Type;
} // namespace llvm

namespace lgc {

class PipelineState;

// =====================================================================================================================
// Struct with the information for one vertex fetch
struct VertexFetchInfo {
  unsigned location;
  unsigned component;
  llvm::Type *ty;
};

// =====================================================================================================================
// Struct with information on wave dispatch SGPRs and VGPRs for VS, written by getVsEntryRegInfo
struct VsEntryRegInfo {
  unsigned callingConv;       // Which hardware shader the VS is in (as CallingConv::ID)
  unsigned vertexBufferTable; // SGPR for vertex buffer table
  unsigned baseVertex;        // SGPR for base vertex
  unsigned baseInstance;      // SGPR for base instance
  unsigned sgprCount;         // Total SGPRs at wave dispatch (possibly conservative)
  unsigned vertexId;          // VGPR for vertex ID
  unsigned instanceId;        // VGPR for instance ID
  unsigned vgprCount;         // Total VGPRs at wave dispatch (exact)
  bool wave32;                // Whether VS is wave32
};

// =====================================================================================================================
// Struct with the information for one color export
struct ColorExportInfo {
  unsigned hwColorTarget;
  unsigned location;
  bool isSigned;
  llvm::Type *ty;
};

// =====================================================================================================================
// Struct containing the FS input mappings, generated and stored in PAL metadata when compiling an FS by itself,
// and consumed when generating the rest-of-pipeline that will link to it.
struct FsInputMappings {
  // For each input, the original InOutLocationInfo and the mapped InOutLocationInfo.
  // An InOutLocationInfo contains bitfields for the location number, component number, and a few other
  // things.
  llvm::SmallVector<std::pair<unsigned, unsigned>> locationInfo;
  // For each built-in input that is implemented as a generic input passed from the previous shader stage,
  // such as CullDistance and ClipDistance, the built-in id and the mapped location number.
  llvm::SmallVector<std::pair<unsigned, unsigned>> builtInLocationInfo;
};

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
  void mergeFromBlob(llvm::StringRef blob, bool isGlueCode);

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

  // Get a register value in PAL metadata.
  unsigned getRegister(unsigned regNum);

  // Set a register value in PAL metadata. If the register is already set, this ORs in the value.
  void setRegister(unsigned regNum, unsigned value);

  // Store the vertex fetch in PAL metadata for a fetchless vertex shader with shader compilation.
  void addVertexFetchInfo(llvm::ArrayRef<VertexFetchInfo> fetches);

  // Get the count of vertex fetches for a fetchless vertex shader with shader compilation (or 0 otherwise).
  unsigned getVertexFetchCount();

  // Get the vertex fetch information out of PAL metadata
  void getVertexFetchInfo(llvm::SmallVectorImpl<VertexFetchInfo> &fetches);

  // Get the VS entry register info. Used by the linker to generate the fetch shader.
  void getVsEntryRegInfo(VsEntryRegInfo &regInfo);

  // Store the color export info in the PAL metadata
  void addColorExportInfo(llvm::ArrayRef<ColorExportInfo> exports);

  // Get the count of vertex fetches for a fetchless vertex shader with shader compilation (or 0 otherwise).
  unsigned getColorExportCount();

  // Get the vertex fetch information out of PAL metadata
  void getColorExportInfo(llvm::SmallVectorImpl<ColorExportInfo> &exports);

  // Erase the color export info
  void eraseColorExportInfo();

  // Finalize PAL metadata for pipeline, part-pipeline or shader compilation.
  void finalizePipeline(bool isWholePipeline);

  // Updates the PS register information that depends on the exports.
  void updateSpiShaderColFormat(llvm::ArrayRef<ColorExportInfo> exps, bool hasDepthExpFmtZero, bool killEnabled);

  // Sets the finalized 128-bit cache hash.  The version identifies the version of LLPC used to generate the hash.
  void setFinalized128BitCacheHash(const lgc::Hash128 &finalizedCacheHash, const llvm::VersionTuple &version);

  // Store the fragment shader input mapping information for a fragment shader being compiled by itself (partial
  // pipeline compilation).
  void addFragmentInputInfo(const FsInputMappings &fsInputMappings);

  // Check whether we have FS input mappings, and thus whether we're doing part-pipeline compilation of the
  // pre-FS part of the pipeline.
  bool haveFsInputMappings();

  // In part-pipeline compilation, get a blob of data representing the FS input mappings that can be used by the
  // client in a hash. The resulting data is owned by the PalMetadata object, and lives until the PalMetadata
  // object is destroyed or another call is made to getFsInputMappings.
  llvm::StringRef getFsInputMappings();

  // In part-pipeline compilation, retrieve the FS input mappings into the provided vectors, and delete them
  // from the PAL metadata so they do not appear in the final ELF.
  void retrieveFragmentInputInfo(FsInputMappings &fsInputMappings);

  // In part-pipeline compilation, copy any metadata needed from the "other" pipeline's PAL metadata into ours.
  void setOtherPartPipeline(PalMetadata &other);

  // Erase the PAL metadata for FS input mappings. Used when finalizing the PAL metadata in the link.
  void eraseFragmentInputInfo();

  // Returns true if the fragment input info has an entry for a builtin.
  bool fragmentShaderUsesMappedBuiltInInputs();

private:
  // Initialize the PalMetadata object after reading in already-existing PAL metadata if any
  void initialize();

  // Get the first user data register number for the given shader stage.
  unsigned getUserDataReg0(ShaderStage stage);

  // Get the llvm type that corresponds to tyName.  Returns nullptr if no such type exists.
  llvm::Type *getLlvmType(llvm::StringRef tyName) const;

  // Set userDataLimit to maximum
  void setUserDataLimit();

  // Returns true of the some of the user data nodes are spilled.
  bool userDataNodesAreSpilled() const { return m_spillThreshold->getUInt() != MAX_SPILL_THRESHOLD; }

  // Test whether this is a graphics pipeline (even works in a link-only pipeline).
  bool isGraphics();

  // Finalize PAL metadata user data limit for any compilation (shader, part-pipeline, whole pipeline)
  void finalizeUserDataLimit();

  // The maximum possible value for the spill threshold entry in the PAL meatadata.
  static constexpr uint64_t MAX_SPILL_THRESHOLD = UINT_MAX;

  unsigned getUserDataCount(unsigned callingConv);
  unsigned getCallingConventionForFirstHardwareShaderStage();
  unsigned getFirstUserDataReg(unsigned callingConv);
  unsigned getNumberOfSgprsBeforeUserData(unsigned conv);
  unsigned getOffsetOfUserDataReg(std::map<llvm::msgpack::DocNode, llvm::msgpack::DocNode>::iterator firstUserDataNode,
                                  UserDataMapping userDataMapping);
  unsigned getNumberOfSgprsAfterUserData(unsigned callingConv);
  unsigned getVertexIdOffset(unsigned callingConv);
  unsigned getInstanceIdOffset(unsigned callingConv);
  unsigned getVgprCount(unsigned callingConv);
  bool isWave32(unsigned callingConv);

  PipelineState *m_pipelineState;             // PipelineState
  llvm::msgpack::Document *m_document;        // The MsgPack document
  llvm::msgpack::MapDocNode m_pipelineNode;   // MsgPack map node for amdpal.pipelines[0]
  llvm::msgpack::MapDocNode m_registers;      // MsgPack map node for amdpal.pipelines[0].registers
  llvm::msgpack::ArrayDocNode m_vertexInputs; // MsgPack map node for amdpal.pipelines[0].vertexInputs
  llvm::msgpack::DocNode m_colorExports;      // MsgPack map node for amdpal.pipelines[0].colorExports
  // Mapping from ShaderStage to SPI user data register start, allowing for merged shaders and NGG.
  unsigned m_userDataRegMapping[ShaderStageCountInternal] = {};
  llvm::msgpack::DocNode *m_userDataLimit;  // Maximum so far number of user data dwords used
  llvm::msgpack::DocNode *m_spillThreshold; // Minimum so far dword offset used in user data spill table
  llvm::SmallString<0> m_fsInputMappingsBlob; // Buffer for returning FS input mappings blob to LGC client
};

} // namespace lgc
