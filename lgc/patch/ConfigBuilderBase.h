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
 * @file  ConfigBuilderBase.h
 * @brief LLPC header file: contains declaration of class lgc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

namespace llvm {

class LLVMContext;
class Module;

} // namespace llvm

namespace lgc {

class PipelineState;

// Invalid metadata key and value which shouldn't be exported to ELF.
constexpr unsigned InvalidMetadataKey = 0xFFFFFFFF;
constexpr unsigned InvalidMetadataValue = 0xBAADBEEF;

struct PalMetadataNoteEntry {
  unsigned key;
  unsigned value;
};

// =====================================================================================================================
// Register configuration builder base class.
class ConfigBuilderBase {
public:
  ConfigBuilderBase(llvm::Module *module, PipelineState *pipelineState);
  ~ConfigBuilderBase();

  void writePalMetadata();

protected:
  void addApiHwShaderMapping(ShaderStage apiStage, unsigned hwStages);

  unsigned setShaderHash(ShaderStage apiStage);
  void setNumAvailSgprs(Util::Abi::HardwareStage hwStage, unsigned value);
  void setNumAvailVgprs(Util::Abi::HardwareStage hwStage, unsigned value);
  void setUsesViewportArrayIndex(bool useViewportIndex);
  void setPsUsesUavs(bool value);
  void setPsWritesUavs(bool value);
  void setPsWritesDepth(bool value);
  void setEsGsLdsByteSize(unsigned value);
  void setCalcWaveBreakSizeAtDrawTime(bool value);
  void setWaveFrontSize(Util::Abi::HardwareStage hwStage, unsigned value);
  void setApiName(const char *value);
  void setPipelineType(Util::Abi::PipelineType value);
  void setLdsSizeByteSize(Util::Abi::HardwareStage hwStage, unsigned value);
  void setEsGsLdsSize(unsigned value);
  unsigned setupFloatingPointMode(ShaderStage shaderStage);

  void appendConfig(llvm::ArrayRef<PalMetadataNoteEntry> config);
  void appendConfig(unsigned key, unsigned value);

  template <typename T> void appendConfig(const T &config) {
    static_assert(T::ContainsPalAbiMetadataOnly, "may only be used with structs that are fully metadata notes");
    static_assert(sizeof(T) % sizeof(PalMetadataNoteEntry) == 0,
                  "T claims to be isPalAbiMetadataOnly, but sizeof contradicts that");

    appendConfig({reinterpret_cast<const PalMetadataNoteEntry *>(&config), sizeof(T) / sizeof(PalMetadataNoteEntry)});
  }

  // -----------------------------------------------------------------------------------------------------------------

  llvm::Module *m_module;         // LLVM module being processed
  llvm::LLVMContext *m_context;   // LLVM context
  PipelineState *m_pipelineState; // Pipeline state
  GfxIpVersion m_gfxIp;           // Graphics IP version info

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader

  unsigned m_userDataLimit;  // User data limit for shaders seen so far
  unsigned m_spillThreshold; // Spill threshold for shaders seen so far

private:
  // Get the MsgPack map node for the specified API shader in the ".shaders" map
  llvm::msgpack::MapDocNode getApiShaderNode(unsigned apiStage);
  // Get the MsgPack map node for the specified HW shader in the ".hardware_stages" map
  llvm::msgpack::MapDocNode getHwShaderNode(Util::Abi::HardwareStage hwStage);
  // Set USER_DATA_LIMIT (called once for the whole pipeline)
  void setUserDataLimit();
  // Set SPILL_THRESHOLD (called once for the whole pipeline)
  void setSpillThreshold();
  // Set PIPELINE_HASH (called once for the whole pipeline)
  void setPipelineHash();

  // -----------------------------------------------------------------------------------------------------------------
  llvm::msgpack::Document *m_document;                 // The MsgPack document
  llvm::msgpack::MapDocNode m_pipelineNode;            // MsgPack map node for amdpal.pipelines[0]
  llvm::msgpack::MapDocNode m_apiShaderNodes[ShaderStageNativeStageCount];
  // MsgPack map node for each API shader's node in
  //  ".shaders"
  llvm::msgpack::MapDocNode m_hwShaderNodes[unsigned(Util::Abi::HardwareStage::Count)];
  // MsgPack map node for each HW shader's node in
  //  ".hardware_stages"

  llvm::SmallVector<PalMetadataNoteEntry, 128> m_config; // Register/metadata configuration
};

} // namespace lgc
