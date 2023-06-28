/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchEntryPointMutate.h
 * @brief LLPC header file: contains declaration of class lgc::PatchEntryPointMutate.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgccps/LgcCpsDialect.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/TypeLowering.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"

namespace lgc {

// =====================================================================================================================
// The entry-point mutation pass
class PatchEntryPointMutate : public Patch, public llvm::PassInfoMixin<PatchEntryPointMutate> {
public:
  PatchEntryPointMutate();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for entry-point mutation"; }

private:
  // A shader entry-point user data argument
  struct UserDataArg {
    UserDataArg(llvm::Type *argTy, const llvm::Twine &name,
                unsigned userDataValue = static_cast<unsigned>(UserDataMapping::Invalid), unsigned *argIndex = nullptr);

    UserDataArg(llvm::Type *argTy, const llvm::Twine &name, UserDataMapping userDataValue,
                unsigned *argIndex = nullptr);

    llvm::Type *argTy;      // IR type of the argument
    std::string name;       // Name of the argument
    unsigned argDwordSize;  // Size of argument in dwords
    unsigned userDataValue; // PAL metadata user data value, ~0U (UserDataMapping::Invalid) for none
    unsigned *argIndex;     // Where to store arg index once it is allocated, nullptr for none
  };

  // User data usage for one user data node
  struct UserDataNodeUsage {
    unsigned entryArgIdx = 0;
    unsigned dwordSize = 0; // Only used in pushConstOffsets
    llvm::SmallVector<llvm::Instruction *, 4> users;
  };

  // Per-merged-shader-stage gathered user data usage information.
  struct UserDataUsage {
    // Check if special user data value is used by lgc.special.user.data call generated before PatchEntryPointMutate
    bool isSpecialUserDataUsed(UserDataMapping kind);

    // List of lgc.spill.table calls
    UserDataNodeUsage spillTable;
    // List of lgc.push.const calls. There is no direct attempt to unspill these; instead we attempt to
    // unspill the pushConstOffsets loads.
    UserDataNodeUsage pushConst;
    // True means that we did not succeed in putting all loads into pushConstOffsets, so lgc.push.const
    // calls must be kept.
    bool pushConstSpill = false;
    // Per-push-const-offset lists of loads from push const. We attempt to unspill these.
    llvm::SmallVector<UserDataNodeUsage, 8> pushConstOffsets;
    // Per-user-data-offset lists of lgc.root.descriptor calls
    llvm::SmallVector<UserDataNodeUsage, 8> rootDescriptors;
    // Per-table lists of lgc.descriptor.table.addr calls
    // When the user data nodes are available, a table is identifed by its
    // index in the user data nodes.  Using this index allows for the possibility that a descriptor
    // set is split over multiple tables.  When it is not available, a table is identified by the
    // descriptor set it contains, which is consistent with the Vulkan binding model.
    llvm::SmallVector<UserDataNodeUsage, 8> descriptorTables;
    // Per-UserDataMapping lists of lgc.special.user.data calls
    llvm::SmallVector<UserDataNodeUsage, 18> specialUserData;
    // Minimum offset at which spill table is used.
    unsigned spillUsage = UINT_MAX;
    // Usage of streamout table
    bool usesStreamOutTable = false;
  };

  // Set up compute-with-calls flag.
  void setupComputeWithCalls(llvm::Module *module);

  // Gather user data usage in all shaders.
  void gatherUserDataUsage(llvm::Module *module);

  // Fix up user data uses.
  void fixupUserDataUses(llvm::Module &module);

  void processShader(ShaderInputs *shaderInputs);
  void processComputeFuncs(ShaderInputs *shaderInputs, llvm::Module &module);
  void processCalls(llvm::Function &func, llvm::SmallVectorImpl<llvm::Type *> &shaderInputTys,
                    llvm::SmallVectorImpl<std::string> &shaderInputNames, uint64_t inRegMask, unsigned argOffset);
  void setFuncAttrs(llvm::Function *entryPoint);

  uint64_t generateEntryPointArgTys(ShaderInputs *shaderInputs, llvm::Function *origFunc,
                                    llvm::SmallVectorImpl<llvm::Type *> &argTys,
                                    llvm::SmallVectorImpl<std::string> &argNames, unsigned argOffset);

  bool isSystemUserDataValue(unsigned userDataValue) const;
  bool isUnlinkedDescriptorSetValue(unsigned value) const;

  void addSpecialUserDataArgs(llvm::SmallVectorImpl<UserDataArg> &userDataArgs,
                              llvm::SmallVectorImpl<UserDataArg> &specialUserDataArgs, llvm::IRBuilder<> &builder);
  void addUserDataArgs(llvm::SmallVectorImpl<UserDataArg> &userDataArgs, llvm::IRBuilder<> &builder);
  void addUserDataArg(llvm::SmallVectorImpl<UserDataArg> &userDataArgs, unsigned userDataValue, unsigned sizeInDwords,
                      const llvm::Twine &name, unsigned *argIndex, llvm::IRBuilder<> &builder);

  void determineUnspilledUserDataArgs(llvm::ArrayRef<UserDataArg> userDataArgs,
                                      llvm::ArrayRef<UserDataArg> specialUserDataArgs, llvm::IRBuilder<> &builder,
                                      llvm::SmallVectorImpl<UserDataArg> &unspilledArgs);

  uint64_t pushFixedShaderArgTys(llvm::SmallVectorImpl<llvm::Type *> &argTys) const;

  // Information about each cps exit (return or cps.jump) used for exit unification.
  struct CpsExitInfo {
    CpsExitInfo(llvm::BasicBlock *_pred, llvm::SmallVector<llvm::Value *> _vgpr) : pred(_pred), vgpr(_vgpr) {}
    llvm::BasicBlock *pred;                // The predecessor that will branch to the unified exit.
    llvm::SmallVector<llvm::Value *> vgpr; // The vgpr values from the exit.
  };

  bool lowerCpsOps(llvm::Function *func);
  llvm::Function *lowerCpsFunction(llvm::Function *func, llvm::ArrayRef<llvm::Type *> userDataTys,
                                   llvm::ArrayRef<std::string> argNames);
  void lowerCpsJump(llvm::Function *parent, cps::JumpOp *jumpOp, llvm::BasicBlock *tailBlock,
                    llvm::SmallVectorImpl<CpsExitInfo> &exitInfos);
  void lowerAsCpsReference(cps::AsContinuationReferenceOp &asCpsReferenceOp);

  // Get UserDataUsage struct for the merged shader stage that contains the given shader stage
  UserDataUsage *getUserDataUsage(ShaderStage stage);

  // Get the shader stage that the given shader stage is merged into.
  ShaderStage getMergedShaderStage(ShaderStage stage) const;

  bool isComputeWithCalls() const;

  bool m_hasTs;                             // Whether the pipeline has tessllation shader
  bool m_hasGs;                             // Whether the pipeline has geometry shader
  PipelineState *m_pipelineState = nullptr; // Pipeline state from PipelineStateWrapper pass
  bool m_computeWithCalls = false;          // Whether this is compute pipeline with calls or compute library
  // Per-HW-shader-stage gathered user data usage information.
  llvm::SmallVector<std::unique_ptr<UserDataUsage>, ShaderStageCount> m_userDataUsage;

  class CpsShaderInputCache {
  public:
    void clear() {
      if (m_cpsCacheAvailable) {
        m_cpsShaderInputTypes.clear();
        m_cpsShaderInputNames.clear();
        m_cpsCacheAvailable = false;
      }
    }
    void set(llvm::ArrayRef<llvm::Type *> types, llvm::ArrayRef<std::string> names) {
      assert(!m_cpsCacheAvailable);
      m_cpsCacheAvailable = true;
      m_cpsShaderInputTypes.append(types.begin(), types.end());
      m_cpsShaderInputNames.append(names.begin(), names.end());
    }
    llvm::ArrayRef<llvm::Type *> getTypes() { return m_cpsShaderInputTypes; }
    llvm::ArrayRef<std::string> getNames() { return m_cpsShaderInputNames; }
    bool isAvailable() { return m_cpsCacheAvailable; }

  private:
    llvm::SmallVector<llvm::Type *> m_cpsShaderInputTypes;
    llvm::SmallVector<std::string> m_cpsShaderInputNames;
    bool m_cpsCacheAvailable = false;
  };
  CpsShaderInputCache m_cpsShaderInputCache;
  // Map from a cps function to the alloca where we are holding the latest continuation stack pointer.
  llvm::DenseMap<llvm::Function *, llvm::Value *> m_funcCpsStackMap;
};

} // namespace lgc
