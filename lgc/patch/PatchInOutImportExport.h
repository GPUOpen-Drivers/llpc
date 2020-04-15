/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchInOutImportExport.h
 * @brief LLPC header file: contains declaration of class lgc::PatchInOutImportExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "SystemValues.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/InstVisitor.h"
#include <set>

namespace lgc {

class FragColorExport;
class VertexFetch;

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for input import and output export.
class PatchInOutImportExport : public Patch, public llvm::InstVisitor<PatchInOutImportExport> {
public:
  PatchInOutImportExport();
  ~PatchInOutImportExport();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addPreserved<PipelineShaders>();
  }

  bool runOnModule(llvm::Module &module) override;
  void visitCallInst(llvm::CallInst &callInst);
  void visitReturnInst(llvm::ReturnInst &retInst);

  // -----------------------------------------------------------------------------------------------------------------

  static char ID; // ID of this pass

private:
  PatchInOutImportExport(const PatchInOutImportExport &) = delete;
  PatchInOutImportExport &operator=(const PatchInOutImportExport &) = delete;

  void initPerShader();

  void processShader();

  llvm::Value *patchVsGenericInputImport(llvm::Type *inputTy, unsigned location, unsigned compIdx,
                                         llvm::Instruction *insertPos);
  llvm::Value *patchTcsGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                          llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  llvm::Value *patchTesGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                          llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  llvm::Value *patchGsGenericInputImport(llvm::Type *inputTy, unsigned location, unsigned compIdx,
                                         llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  llvm::Value *patchFsGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                         llvm::Value *compIdx, llvm::Value *auxInterpValue, unsigned interpMode,
                                         unsigned interpLoc, llvm::Instruction *insertPos, bool highHalf = false);

  llvm::Value *patchTcsGenericOutputImport(llvm::Type *outputTy, unsigned location, llvm::Value *locOffset,
                                           llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  void patchVsGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx,
                                  llvm::Instruction *insertPos);
  void patchTcsGenericOutputExport(llvm::Value *output, unsigned location, llvm::Value *locOffset, llvm::Value *compIdx,
                                   llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  void patchTesGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx,
                                   llvm::Instruction *insertPos);
  void patchGsGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                                  llvm::Instruction *insertPos);
  void patchFsGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx,
                                  llvm::Instruction *insertPos);

  llvm::Value *patchVsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Instruction *insertPos);
  llvm::Value *patchTcsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *elemIdx,
                                          llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  llvm::Value *patchTesBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *elemIdx,
                                          llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  llvm::Value *patchGsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *vertexIdx,
                                         llvm::Instruction *insertPos);
  llvm::Value *patchFsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *sampleId,
                                         llvm::Instruction *insertPos);
  llvm::Value *getSamplePosOffset(llvm::Type *inputTy, llvm::Value *sampleId, llvm::Instruction *insertPos);
  llvm::Value *getSamplePosition(llvm::Type *inputTy, llvm::Instruction *insertPos);
  llvm::Value *patchCsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Instruction *insertPos);
  llvm::Value *getGlobalInvocationId(llvm::Type *inputTy, llvm::Instruction *insertPos);
  llvm::Value *getLocalInvocationIndex(llvm::Type *inputTy, llvm::Instruction *insertPos);
  llvm::Value *getSubgroupId(llvm::Type *inputTy, llvm::Instruction *insertPos);

  llvm::Value *patchTcsBuiltInOutputImport(llvm::Type *outputTy, unsigned builtInId, llvm::Value *elemIdx,
                                           llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  void patchVsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);
  void patchTcsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Value *elemIdx,
                                   llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  void patchTesBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);
  void patchGsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, unsigned streamId,
                                  llvm::Instruction *insertPos);
  void patchFsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  void patchCopyShaderGenericOutputExport(llvm::Value *output, unsigned location, llvm::Instruction *insertPos);
  void patchCopyShaderBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  void patchXfbOutputExport(llvm::Value *output, unsigned xfbBuffer, unsigned xfbOffset, unsigned locOffset,
                            llvm::Instruction *insertPos);

  void storeValueToStreamOutBuffer(llvm::Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset, unsigned xfbStride,
                                   llvm::Value *streamOutBufDesc, llvm::Instruction *insertPos);

  void createStreamOutBufferStoreFunction(llvm::Value *storeValue, unsigned xfbStrde, std::string &funcName);

  unsigned combineBufferStore(const std::vector<llvm::Value *> &storeValues, unsigned startIdx, unsigned valueOffset,
                              llvm::Value *bufDesc, llvm::Value *storeOffset, llvm::Value *bufBase,
                              CoherentFlag coherent, llvm::Instruction *insertPos);

  unsigned combineBufferLoad(std::vector<llvm::Value *> &loadValues, unsigned startIdx, llvm::Value *bufDesc,
                             llvm::Value *loadOffset, llvm::Value *bufBase, CoherentFlag coherent,
                             llvm::Instruction *insertPos);

  void storeValueToEsGsRing(llvm::Value *storeValue, unsigned location, unsigned compIdx, llvm::Instruction *insertPos);

  llvm::Value *loadValueFromEsGsRing(llvm::Type *loadType, unsigned location, unsigned compIdx, llvm::Value *vertexIdx,
                                     llvm::Instruction *insertPos);

  void storeValueToGsVsRing(llvm::Value *storeValue, unsigned location, unsigned compIdx, unsigned streamId,
                            llvm::Instruction *insertPos);

  llvm::Value *calcEsGsRingOffsetForOutput(unsigned location, unsigned compIdx, llvm::Value *esGsOffset,
                                           llvm::Instruction *insertPos);

  llvm::Value *calcEsGsRingOffsetForInput(unsigned location, unsigned compIdx, llvm::Value *vertexIdx,
                                          llvm::Instruction *insertPos);

  llvm::Value *calcGsVsRingOffsetForOutput(unsigned location, unsigned compIdx, unsigned streamId,
                                           llvm::Value *vertexIdx, llvm::Value *gsVsOffset,
                                           llvm::Instruction *insertPos);

  llvm::Value *readValueFromLds(bool isOutput, llvm::Type *readTy, llvm::Value *ldsOffset,
                                llvm::Instruction *insertPos);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset, llvm::Instruction *insertPos);

  llvm::Value *calcTessFactorOffset(bool isOuter, llvm::Value *elemIdx, llvm::Instruction *insertPos);

  void storeTessFactorToBuffer(const std::vector<llvm::Value *> &tessFactors, llvm::Value *tessFactorOffset,
                               llvm::Instruction *insertPos);

  void createTessBufferStoreFunction();

  unsigned calcPatchCountPerThreadGroup(unsigned inVertexCount, unsigned inVertexStride, unsigned outVertexCount,
                                        unsigned outVertexStride, unsigned patchConstCount,
                                        unsigned tessFactorStride) const;

  llvm::Value *calcLdsOffsetForVsOutput(llvm::Type *outputTy, unsigned location, unsigned compIdx,
                                        llvm::Instruction *insertPos);

  llvm::Value *calcLdsOffsetForTcsInput(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                        llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  llvm::Value *calcLdsOffsetForTcsOutput(llvm::Type *outputTy, unsigned location, llvm::Value *locOffset,
                                         llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  llvm::Value *calcLdsOffsetForTesInput(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                        llvm::Value *compIdx, llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  void addExportInstForGenericOutput(llvm::Value *output, unsigned location, unsigned compIdx,
                                     llvm::Instruction *insertPos);
  void addExportInstForBuiltInOutput(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  llvm::Value *adjustCentroidIj(llvm::Value *centroidIj, llvm::Value *centerIj, llvm::Instruction *insertPos);

  llvm::Value *getSubgroupLocalInvocationId(llvm::Instruction *insertPos);

  WorkgroupLayout calculateWorkgroupLayout();
  llvm::Value *reconfigWorkgroup(llvm::Value *localInvocationId, llvm::Instruction *insertPos);
  llvm::Value *getWorkgroupSize();
  llvm::Value *getInLocalInvocationId(llvm::Instruction *insertPos);

  // -----------------------------------------------------------------------------------------------------------------

  GfxIpVersion m_gfxIp;                     // Graphics IP version info
  PipelineSystemValues m_pipelineSysValues; // Cache of ShaderSystemValues objects, one per shader stage

  VertexFetch *m_vertexFetch;         // Vertex fetch manager
  FragColorExport *m_fragColorExport; // Fragment color export manager

  llvm::CallInst *m_lastExport; // Last "export" intrinsic for which "done" flag is valid

  llvm::Value *m_clipDistance; // Correspond to "out float gl_ClipDistance[]"
  llvm::Value *m_cullDistance; // Correspond to "out float gl_CullDistance[]"
  llvm::Value *m_primitiveId;  // Correspond to "out int gl_PrimitiveID"
  // NOTE: gl_FragDepth, gl_FragStencilRef and gl_SampleMask[] are exported at the same time with one "EXP"
  // instruction. Thus, the export is delayed.
  llvm::Value *m_fragDepth;      // Correspond to "out float gl_FragDepth"
  llvm::Value *m_fragStencilRef; // Correspond to "out int gl_FragStencilRef"
  llvm::Value *m_sampleMask;     // Correspond to "out int gl_SampleMask[]"
  // NOTE: For GFX9, gl_ViewportIndex and gl_Layer are packed with one channel (gl_ViewpoertInex is 16-bit high part
  // and gl_Layer is 16-bit low part). Thus, the export is delayed with them merged together.
  llvm::Value *m_viewportIndex; // Correspond to "out int gl_ViewportIndex"
  llvm::Value *m_layer;         // Correspond to "out int gl_Layer"

  bool m_hasTs; // Whether the pipeline has tessellation shaders

  bool m_hasGs; // Whether the pipeline has geometry shader

  llvm::GlobalVariable *m_lds; // Global variable to model LDS
  llvm::Value *m_threadId;     // Thread ID

  std::vector<llvm::Value *> m_expFragColors[MaxColorTargets]; // Exported fragment colors
  std::vector<llvm::CallInst *> m_importCalls;                 // List of "call" instructions to import inputs
  std::vector<llvm::CallInst *> m_exportCalls;                 // List of "call" instructions to export outputs
  PipelineState *m_pipelineState = nullptr;                    // Pipeline state from PipelineStateWrapper pass

  std::set<unsigned> m_expLocs; // The locations that already have an export instruction for the vertex shader.
};

} // namespace lgc
