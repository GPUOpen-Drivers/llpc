/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  FragColorExport.h
 * @brief LLPC header file: contains declaration of class lgc::FragColorExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace lgc {

class PipelineState;
struct WorkaroundFlags;

// Enumerates component setting of color format. This is a "helper" enum used in the CB's algorithm for deriving
// an ideal shader export format.
enum class CompSetting : unsigned {
  Invalid,         // Invalid
  OneCompRed,      // Red
  OneCompAlpha,    // Alpha
  TwoCompAlphaRed, // Alpha, red
  TwoCompGreenRed  // Green, red
};

// =====================================================================================================================
// Represents the manager of fragment color export operations.
class FragColorExport {
public:
  // Color export Info
  struct Key {
    ColorExportState colorExportState;          // Color export state
    unsigned channelWriteMask[MaxColorTargets]; // Write mask to specify destination channels
    unsigned expFmt[MaxColorTargets];           // Export format used for "export" instruction.
    unsigned waveSize;                          // The wave size for fragment.
    bool enableFragColor;                       // Whether to broadcast frag color. Only for OGLP
  };

  FragColorExport(LgcContext *context);

  void generateExportInstructions(llvm::ArrayRef<lgc::ColorExportInfo> info, llvm::ArrayRef<llvm::Value *> values,
                                  bool dummyExport, PalMetadata *palMetadata, BuilderBase &builder,
                                  llvm::Value *dynamicIsDualSource, const Key &key);
  static void setDoneFlag(llvm::Value *exportInst, BuilderBase &builder);
  static llvm::CallInst *addDummyExport(BuilderBase &builder);
  static llvm::Function *generateNullFragmentShader(llvm::Module &module, PipelineState *pipelineState,
                                                    llvm::StringRef entryPointName);
  static llvm::Function *generateNullFragmentEntryPoint(llvm::Module &module, PipelineState *pipelineState,
                                                        llvm::StringRef entryPointName);
  static void generateNullFragmentShaderBody(llvm::Function *entryPoint);

  static Key computeKey(llvm::ArrayRef<ColorExportInfo> info, PipelineState *pipelineState);

private:
  FragColorExport() = delete;
  FragColorExport(const FragColorExport &) = delete;
  FragColorExport &operator=(const FragColorExport &) = delete;
  void updateColorExportInfoWithBroadCastInfo(const Key &key, llvm::ArrayRef<ColorExportInfo> originExpinfo,
                                              bool needMrt0a, llvm::SmallVector<ColorExportInfo> &outExpinfo,
                                              unsigned *pCbShaderMask);
  llvm::Value *handleColorExportInstructions(llvm::Value *output, unsigned int hwColorExport, BuilderBase &builder,
                                             ExportFormat expFmt, const bool signedness, const bool isDualSourceBlend);
  llvm::Value *convertToHalf(llvm::Value *value, bool signedness, BuilderBase &builder) const;
  llvm::Value *convertToFloat(llvm::Value *value, bool signedness, BuilderBase &builder) const;
  llvm::Value *convertToInt(llvm::Value *value, bool signedness, BuilderBase &builder) const;

  llvm::Value *dualSourceSwizzle(unsigned waveSize, BuilderBase &builder);

  // Colors to be exported for dual-source-blend
  llvm::SmallVector<llvm::Value *, 4> m_blendSources[2];
  // Number of color channels for dual-source-blend
  unsigned m_blendSourceChannels = 0;

  LgcContext *m_lgcContext;
};

// The information needed for an export to a hardware color target.
struct ColorOutputValueInfo {
  std::array<llvm::Value *, 4> value; // The value of each component to be exported.
  bool isSigned;                      // True if the values should be interpreted as signed integers.
};

// =====================================================================================================================
// Pass to lower color export calls
class LowerFragColorExport : public llvm::PassInfoMixin<LowerFragColorExport> {
public:
  LowerFragColorExport();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower fragment color export calls"; }

private:
  void updateFragColors(llvm::CallInst *callInst, llvm::MutableArrayRef<ColorOutputValueInfo> outFragColors,
                        BuilderBase &builder);
  void collectExportInfoForGenericOutputs(llvm::Function *fragEntryPoint, BuilderBase &builder);
  void collectExportInfoForBuiltinOutput(llvm::Function *module, BuilderBase &builder);
  llvm::Value *generateValueForOutput(llvm::Value *value, llvm::Type *outputTy, BuilderBase &builder);
  void createTailJump(llvm::Function *fragEntryPoint, BuilderBase &builder, llvm::Value *isDualSource);

  llvm::LLVMContext *m_context;                        // The context the pass is being run in.
  PipelineState *m_pipelineState;                      // The pipeline state
  ResourceUsage *m_resUsage;                           // The resource usage object from the pipeline state.
  llvm::SmallVector<ColorExportInfo, 8> m_info;        // The color export information for each export.
  llvm::SmallVector<llvm::Value *, 10> m_exportValues; // The value to be exported indexed by the hw render target.
};

} // namespace lgc
