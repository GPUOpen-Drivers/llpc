/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Function.h"
#include <set>

namespace lgc {

// =====================================================================================================================
// Represents the pass of LLVM patching operations for input import and output export.
class PatchInOutImportExport : public Patch, public llvm::PassInfoMixin<PatchInOutImportExport> {
public:
  PatchInOutImportExport();

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState,
               const std::function<llvm::PostDominatorTree &(llvm::Function &)> &getPostDominatorTree);

  static llvm::StringRef name() { return "Patch LLVM for input import and output export operations"; }

  void visitCallInst(llvm::CallInst &callInst);
  void visitReturnInst(llvm::ReturnInst &retInst);

private:
  void processFunction(llvm::Function &func, ShaderStage shaderStage,
                       llvm::SmallVectorImpl<llvm::Function *> &inputCallees,
                       llvm::SmallVectorImpl<llvm::Function *> &otherCallees,
                       const std::function<llvm::PostDominatorTree &(llvm::Function &)> &getPostDominatorTree);
  void initPerShader();

  void markExportDone(llvm::Function *func, llvm::PostDominatorTree &postDomTree);
  void processShader();
  void visitCallInsts(llvm::ArrayRef<llvm::Function *> calleeFuncs);
  void visitReturnInsts();

  llvm::Value *patchTcsGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                          llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);
  llvm::Value *patchTesGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                          llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);
  llvm::Value *patchGsGenericInputImport(llvm::Type *inputTy, unsigned location, unsigned compIdx,
                                         llvm::Value *vertexIdx, BuilderBase &builder);

  llvm::Value *performFsFloatInterpolation(BuilderBase &builder, llvm::Value *attr, llvm::Value *channel,
                                           llvm::Value *coordI, llvm::Value *coordJ, llvm::Value *primMask);
  llvm::Value *performFsHalfInterpolation(BuilderBase &builder, llvm::Value *attr, llvm::Value *channel,
                                          llvm::Value *coordI, llvm::Value *coordJ, llvm::Value *primMask,
                                          llvm::Value *highHalf);

  llvm::Value *performFsParameterLoad(BuilderBase &builder, llvm::Value *attr, llvm::Value *channel,
                                      InterpParam interpParam, llvm::Value *primMask, unsigned bitWidth, bool highHalf);

  llvm::Value *patchFsGenericInputImport(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                         llvm::Value *compIdx, bool isPerPrimitive, unsigned interpMode,
                                         llvm::Value *interpValue, bool highHalf, BuilderBase &builder);

  llvm::Value *patchTcsGenericOutputImport(llvm::Type *outputTy, unsigned location, llvm::Value *locOffset,
                                           llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);

  void patchVsGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx, BuilderBase &builder);
  void patchTcsGenericOutputExport(llvm::Value *output, unsigned location, llvm::Value *locOffset, llvm::Value *compIdx,
                                   llvm::Value *vertexIdx, BuilderBase &builder);
  void patchTesGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx, BuilderBase &builder);
  void patchGsGenericOutputExport(llvm::Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                                  BuilderBase &builder);
  void patchMeshGenericOutputExport(llvm::Value *output, unsigned location, llvm::Value *locOffset,
                                    llvm::Value *compIdx, llvm::Value *vertexOrPrimitiveIdx, bool isPerPrimitive,
                                    BuilderBase &builder);

  llvm::Value *patchTcsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *elemIdx,
                                          llvm::Value *vertexIdx, BuilderBase &builder);
  llvm::Value *patchTesBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *elemIdx,
                                          llvm::Value *vertexIdx, BuilderBase &builder);
  llvm::Value *patchGsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *vertexIdx,
                                         BuilderBase &builder);
  llvm::Value *patchMeshBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, BuilderBase &builder);
  llvm::Value *patchFsBuiltInInputImport(llvm::Type *inputTy, unsigned builtInId, llvm::Value *sampleId,
                                         BuilderBase &builder);
  llvm::Value *getSamplePosOffset(llvm::Type *inputTy, llvm::Value *sampleId, BuilderBase &builder);
  llvm::Value *getSamplePosition(llvm::Type *inputTy, BuilderBase &builder);

  llvm::Value *patchTcsBuiltInOutputImport(llvm::Type *outputTy, unsigned builtInId, llvm::Value *elemIdx,
                                           llvm::Value *vertexIdx, BuilderBase &builder);

  void patchVsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);
  void patchTcsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Value *elemIdx,
                                   llvm::Value *vertexIdx, llvm::Instruction *insertPos);
  void patchTesBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);
  void patchGsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, unsigned streamId, BuilderBase &builder);
  void patchMeshBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Value *elemIdx,
                                    llvm::Value *vertexOrPrimitiveIdx, bool isPerPrimitive,
                                    llvm::Instruction *insertPos);
  void patchFsBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  void patchCopyShaderGenericOutputExport(llvm::Value *output, unsigned location, llvm::Instruction *insertPos);
  void patchCopyShaderBuiltInOutputExport(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  void patchXfbOutputExport(llvm::Value *output, unsigned xfbBuffer, unsigned xfbOffset, unsigned streamId,
                            BuilderBase &builder);

  void storeValueToStreamOutBuffer(llvm::Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset, unsigned xfbStride,
                                   unsigned streamId, BuilderBase &builder);

  unsigned combineBufferStore(const std::vector<llvm::Value *> &storeValues, unsigned startIdx, unsigned valueOffset,
                              llvm::Value *bufDesc, llvm::Value *storeOffset, llvm::Value *bufBase,
                              CoherentFlag coherent, BuilderBase &builder);

  unsigned combineBufferLoad(std::vector<llvm::Value *> &loadValues, unsigned startIdx, llvm::Value *bufDesc,
                             llvm::Value *loadOffset, llvm::Value *bufBase, CoherentFlag coherent,
                             BuilderBase &builder);

  void storeValueToEsGsRing(llvm::Value *storeValue, unsigned location, unsigned compIdx, llvm::Instruction *insertPos);

  llvm::Value *loadValueFromEsGsRing(llvm::Type *loadType, unsigned location, unsigned compIdx, llvm::Value *vertexIdx,
                                     llvm::Instruction *insertPos);

  void storeValueToGsVsRing(llvm::Value *storeValue, unsigned location, unsigned compIdx, unsigned streamId,
                            BuilderBase &builder);

  llvm::Value *calcEsGsRingOffsetForOutput(unsigned location, unsigned compIdx, llvm::Value *esGsOffset,
                                           llvm::Instruction *insertPos);

  llvm::Value *calcEsGsRingOffsetForInput(unsigned location, unsigned compIdx, llvm::Value *vertexIdx,
                                          llvm::Instruction *insertPos);

  llvm::Value *calcGsVsRingOffsetForOutput(unsigned location, unsigned compIdx, unsigned streamId,
                                           llvm::Value *vertexIdx, llvm::Value *gsVsOffset, BuilderBase &builder);

  llvm::Value *readValueFromLds(bool offChip, llvm::Type *readTy, llvm::Value *ldsOffset, BuilderBase &builder);
  void writeValueToLds(bool offChip, llvm::Value *writeValue, llvm::Value *ldsOffset, BuilderBase &builder);

  unsigned calcPatchCountPerThreadGroup(unsigned inVertexCount, unsigned inVertexStride, unsigned outVertexCount,
                                        unsigned outVertexStride, unsigned patchConstCount,
                                        unsigned tessFactorStride) const;

  llvm::Value *calcLdsOffsetForVsOutput(llvm::Type *outputTy, unsigned location, unsigned compIdx,
                                        BuilderBase &builder);

  llvm::Value *calcLdsOffsetForTcsInput(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                        llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);

  llvm::Value *calcLdsOffsetForTcsOutput(llvm::Type *outputTy, unsigned location, llvm::Value *locOffset,
                                         llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);

  llvm::Value *calcLdsOffsetForTesInput(llvm::Type *inputTy, unsigned location, llvm::Value *locOffset,
                                        llvm::Value *compIdx, llvm::Value *vertexIdx, BuilderBase &builder);

  void addExportInstForGenericOutput(llvm::Value *output, unsigned location, unsigned compIdx,
                                     llvm::Instruction *insertPos);
  void addExportInstForBuiltInOutput(llvm::Value *output, unsigned builtInId, llvm::Instruction *insertPos);

  llvm::Value *adjustCentroidIj(llvm::Value *centroidIj, llvm::Value *centerIj, BuilderBase &builder);

  llvm::Value *getSubgroupLocalInvocationId(BuilderBase &builder);

  SwizzleWorkgroupLayout calculateWorkgroupLayout();

  llvm::Value *reconfigWorkgroupLayout(llvm::Value *localInvocationId, WorkgroupLayout macroLayout,
                                       WorkgroupLayout microLayout, unsigned workgroupSizeX, unsigned workgroupSizeY,
                                       unsigned workgroupSizeZ, bool isHwLocalInvocationId,
                                       llvm::Instruction *insertPos);

  void createSwizzleThreadGroupFunction();

  void exportShadingRate(llvm::Value *shadingRate, llvm::Instruction *insertPos);
  llvm::Value *getShadingRate(llvm::Instruction *insertPos);

  void recordVertexAttribExport(unsigned location, llvm::ArrayRef<llvm::Value *> attribValues);
  void exportVertexAttribs(llvm::Instruction *insertPos);

  GfxIpVersion m_gfxIp;                     // Graphics IP version info
  PipelineSystemValues m_pipelineSysValues; // Cache of ShaderSystemValues objects, one per shader stage

  llvm::Value *m_clipDistance; // Correspond to "out float gl_ClipDistance[]"
  llvm::Value *m_cullDistance; // Correspond to "out float gl_CullDistance[]"
  llvm::Value *m_primitiveId;  // Correspond to "out int gl_PrimitiveID"
  // NOTE: gl_FragDepth, gl_FragStencilRef and gl_SampleMask[] are exported at the same time with one "EXP"
  // instruction. Thus, the export is delayed.
  llvm::Value *m_fragDepth;      // Correspond to "out float gl_FragDepth"
  llvm::Value *m_fragStencilRef; // Correspond to "out int gl_FragStencilRef"
  llvm::Value *m_sampleMask;     // Correspond to "out int gl_SampleMask[]"
  // NOTE: For GFX9, gl_ViewportIndex and gl_Layer are packed with one channel (gl_ViewportIndex is 16-bit high part
  // and gl_Layer is 16-bit low part). Thus, the export is delayed with them merged together.
  llvm::Value *m_viewportIndex; // Correspond to "out int gl_ViewportIndex"
  llvm::Value *m_layer;         // Correspond to "out int gl_Layer"
  llvm::Value *m_viewIndex;     // Correspond to "in int gl_ViewIndex"
  llvm::Value *m_edgeFlag;      // Correspond to "EdgeFlag output"

  bool m_hasTs; // Whether the pipeline has tessellation shaders

  bool m_hasGs; // Whether the pipeline has geometry shader

  llvm::GlobalVariable *m_lds; // Global variable to model LDS
  llvm::Value *m_threadId;     // Thread ID

  std::vector<llvm::CallInst *> m_importCalls; // List of "call" instructions to import inputs
  std::vector<llvm::CallInst *> m_exportCalls; // List of "call" instructions to export outputs
  llvm::SmallDenseMap<unsigned, std::array<llvm::Value *, 4>>
      m_attribExports;                      // Export info of vertex attributes: <attrib loc, attrib values>
  PipelineState *m_pipelineState = nullptr; // Pipeline state from PipelineStateWrapper pass

  std::set<unsigned> m_expLocs; // The locations that already have an export instruction for the vertex shader.
  const std::array<unsigned char, 4> *m_buffFormats; // The format of MTBUF instructions for specified GFX
};

} // namespace lgc
