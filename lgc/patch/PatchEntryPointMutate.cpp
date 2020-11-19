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
 * @file  PatchEntryPointMutate.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchEntryPointMutate.
 ***********************************************************************************************************************
 */

#include "lgc/BuilderBase.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-patch-entry-point-mutate"

using namespace llvm;
using namespace lgc;

namespace llvm {
namespace cl {

// -inreg-esgs-lds-size: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent with PAL's
// GS on-chip behavior. In the future, if PAL allows hardcoded ES-GS LDS size, this option could be deprecated.
opt<bool> InRegEsGsLdsSize("inreg-esgs-lds-size", desc("For GS on-chip, add esGsLdsSize in user data"), init(true));

} // namespace cl
} // namespace llvm

namespace {

// =====================================================================================================================
// The entry-point mutation pass
class PatchEntryPointMutate : public Patch {
public:
  PatchEntryPointMutate();
  PatchEntryPointMutate(const PatchEntryPointMutate &) = delete;
  PatchEntryPointMutate &operator=(const PatchEntryPointMutate &) = delete;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    // Does not preserve PipelineShaders because it replaces the entrypoints.
  }

  virtual bool runOnModule(Module &module) override;

  static char ID; // ID of this pass

private:
  // A shader entry-point user data argument
  struct UserDataArg {
    UserDataArg(Type *argTy, unsigned userDataValue = static_cast<unsigned>(UserDataMapping::Invalid),
                unsigned *argIndex = nullptr)
        : argTy(argTy), userDataValue(userDataValue), argIndex(argIndex) {
      if (isa<PointerType>(argTy))
        argDwordSize = argTy->getPointerAddressSpace() == ADDR_SPACE_CONST_32BIT ? 1 : 2;
      else
        argDwordSize = argTy->getPrimitiveSizeInBits() / 32;
    }

    UserDataArg(Type *argTy, UserDataMapping userDataValue, unsigned *argIndex = nullptr)
        : UserDataArg(argTy, static_cast<unsigned>(userDataValue), argIndex) {}

    Type *argTy;            // IR type of the argument
    unsigned argDwordSize;  // Size of argument in dwords
    unsigned userDataValue; // PAL metadata user data value, ~0U (UserDataMapping::Invalid) for none
    unsigned *argIndex;     // Where to store arg index once it is allocated, nullptr for none
  };

  // User data usage for one user data node
  struct UserDataNodeUsage {
    unsigned entryArgIdx = 0;
    unsigned dwordSize = 0; // Only used in pushConstOffsets
    SmallVector<Instruction *, 4> users;
  };

  // Per-merged-shader-stage gathered user data usage information.
  struct UserDataUsage {
    // Check if special user data value is used by lgc.special.user.data call generated before PatchEntryPointMutate
    bool isSpecialUserDataUsed(UserDataMapping kind) {
      unsigned index = static_cast<unsigned>(kind) - static_cast<unsigned>(UserDataMapping::GlobalTable);
      return specialUserData.size() > index && !specialUserData[index].users.empty();
    }

    // List of lgc.spill.table calls
    UserDataNodeUsage spillTable;
    // List of lgc.push.const calls. There is no direct attempt to unspill these; instead we attempt to
    // unspill the pushConstOffsets loads.
    UserDataNodeUsage pushConst;
    // True means that we did not succeed in putting all loads into pushConstOffsets, so lgc.push.const
    // calls must be kept.
    bool pushConstSpill = false;
    // Per-push-const-offset lists of loads from push const. We attempt to unspill these.
    SmallVector<UserDataNodeUsage, 8> pushConstOffsets;
    // Per-user-data-offset lists of lgc.root.descriptor calls
    SmallVector<UserDataNodeUsage, 8> rootDescriptors;
    // Per-descriptor-set lists of lgc.descriptor.set calls
    SmallVector<UserDataNodeUsage, 8> descriptorSets;
    // Per-UserDataMapping lists of lgc.special.user.data calls
    SmallVector<UserDataNodeUsage, 18> specialUserData;
    // Minimum offset at which spill table is used.
    unsigned spillUsage = UINT_MAX;
    // Usage of streamout table
    bool usesStreamOutTable = false;
  };

  // Gather user data usage in all shaders.
  void gatherUserDataUsage(Module *module);

  // Fix up user data uses.
  void fixupUserDataUses(Module &module);

  void processShader(ShaderInputs *shaderInputs);

  uint64_t generateEntryPointArgTys(ShaderInputs *shaderInputs, SmallVectorImpl<Type *> &argTys);

  void addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                              SmallVectorImpl<UserDataArg> &specialUserDataArgs, IRBuilder<> &builder);
  void addUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs, IRBuilder<> &builder);
  void addUserDataArg(SmallVectorImpl<UserDataArg> &userDataArgs, unsigned userDataValue, unsigned sizeInDwords,
                      unsigned *argIndex, IRBuilder<> &builder);

  void determineUnspilledUserDataArgs(ArrayRef<UserDataArg> userDataArgs, ArrayRef<UserDataArg> specialUserDataArgs,
                                      IRBuilder<> &builder, SmallVectorImpl<UserDataArg> &unspilledArgs);

  uint64_t pushFixedShaderArgTys(SmallVectorImpl<Type *> &argTys) const;

  // Get UserDataUsage struct for the merged shader stage that contains the given shader stage
  UserDataUsage *getUserDataUsage(ShaderStage stage);

  // Get the shader stage that the given shader stage is merged into.
  ShaderStage getMergedShaderStage(ShaderStage stage) const;

  bool m_hasTs;                             // Whether the pipeline has tessllation shader
  bool m_hasGs;                             // Whether the pipeline has geometry shader
  PipelineState *m_pipelineState = nullptr; // Pipeline state from PipelineStateWrapper pass
  // Per-HW-shader-stage gathered user data usage information.
  SmallVector<std::unique_ptr<UserDataUsage>, ShaderStageCount> m_userDataUsage;
};

} // anonymous namespace

// =====================================================================================================================
// Initializes static members.
char PatchEntryPointMutate::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for entry-point mutation
ModulePass *lgc::createPatchEntryPointMutate() {
  return new PatchEntryPointMutate();
}

// =====================================================================================================================
PatchEntryPointMutate::PatchEntryPointMutate() : Patch(ID), m_hasTs(false), m_hasGs(false) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchEntryPointMutate::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Entry-Point-Mutate\n");

  Patch::init(&module);

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  // Gather user data usage.
  gatherUserDataUsage(&module);

  // Create ShaderInputs object and gather shader input usage.
  ShaderInputs shaderInputs(&module.getContext());
  shaderInputs.gatherUsage(module);

  // Process each shader in turn, but not the copy shader.
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  for (unsigned shaderStage = ShaderStageVertex; shaderStage < ShaderStageNativeStageCount; ++shaderStage) {
    m_entryPoint = pipelineShaders->getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (m_entryPoint) {
      m_shaderStage = static_cast<ShaderStage>(shaderStage);
      if (m_shaderStage == ShaderStageCompute) {
        // We no longer support compute shader fixed layout required before PAL interface version 624.
        if (m_pipelineState->getLgcContext()->getPalAbiVersion() < 624)
          report_fatal_error("Compute shader not supported before PAL version 624");
      }
      processShader(&shaderInputs);
    }
  }

  // Fix up user data uses to use entry args.
  fixupUserDataUses(*m_module);
  m_userDataUsage.clear();

  // Fix up shader input uses to use entry args.
  shaderInputs.fixupUses(*m_module, m_pipelineState);

  return true;
}

// =====================================================================================================================
// Gather user data usage in all shaders
//
// @param module : IR module
void PatchEntryPointMutate::gatherUserDataUsage(Module *module) {
  // Find lgc.spill.table, lgc.push.constants, lgc.root.descriptor, lgc.descriptor.set functions, and from
  // there all calls to them. Add each call to the applicable list in the UserDataUsage struct for the
  // (merged) shader stage.
  // Find lgc.special.user.data functions, and from there all calls to them. Add each call to the applicable
  // list in the UserDataUsage struct for the (merged) shader stage.
  // Also find lgc.input.import.generic calls in VS, indicating that the vertex buffer table is needed.
  // Also find lgc.output.export.xfb calls anywhere, indicating that the streamout table is needed in the
  // last vertex-processing stage.
  for (Function &func : *module) {
    if (!func.isDeclaration())
      continue;
    if (func.getName().startswith(lgcName::SpillTable)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        getUserDataUsage(stage)->spillTable.users.push_back(call);
      }
      continue;
    }

    if (func.getName().startswith(lgcName::PushConst)) {
      for (User *user : func.users()) {
        // For this call to lgc.push.const, attempt to find all loads with a constant dword-aligned offset and
        // push into userDataUsage->pushConstOffsets. If we fail, set userDataUsage->pushConstSpill to indicate that
        // we need to keep the pointer to the push const, derived as an offset into the spill table.
        CallInst *call = cast<CallInst>(user);
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto userDataUsage = getUserDataUsage(stage);
        userDataUsage->pushConst.users.push_back(call);
        SmallVector<std::pair<Instruction *, unsigned>, 4> users;
        users.push_back({call, 0});
        for (unsigned i = 0; i != users.size(); ++i) {
          Instruction *inst = users[i].first;
          for (User *user : inst->users()) {
            unsigned dwordOffset = users[i].second;
            if (auto bitcast = dyn_cast<BitCastInst>(user)) {
              // See through a bitcast.
              users.push_back({bitcast, dwordOffset});
              continue;
            }
            if (isa<LoadInst>(user) && !user->getType()->isAggregateType()) {
              unsigned byteSize = module->getDataLayout().getTypeStoreSize(user->getType());
              if (byteSize % 4 == 0) {
                // This is a scalar or vector load with dword-aligned size. We can attempt to unspill it, but, for
                // a particular dword offset, we only attempt to unspill ones with the same (minimum) size.
                unsigned dwordSize = byteSize / 4;
                userDataUsage->pushConstOffsets.resize(
                    std::max(unsigned(userDataUsage->pushConstOffsets.size()), dwordOffset + 1));
                auto &pushConstOffset = userDataUsage->pushConstOffsets[dwordOffset];
                if (pushConstOffset.dwordSize == 0 || pushConstOffset.dwordSize >= dwordSize) {
                  if (pushConstOffset.dwordSize != 0 && pushConstOffset.dwordSize != dwordSize) {
                    // This load type is smaller than previously seen ones at this offset. Forget the earlier
                    // ones (and mark that some uses of the push const pointer remain).
                    userDataUsage->pushConstSpill = true;
                    pushConstOffset.users.clear();
                  }
                  // Remember this load for possible unspilling.
                  pushConstOffset.dwordSize = dwordSize;
                  userDataUsage->pushConstOffsets[dwordOffset].users.push_back(cast<Instruction>(user));
                  continue;
                }
              }
            } else if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
              // For a gep, calculate the new constant offset.
              APInt gepOffset(64, 0);
              if (gep->accumulateConstantOffset(module->getDataLayout(), gepOffset)) {
                unsigned gepByteOffset = gepOffset.getZExtValue();
                if (gepByteOffset % 4 == 0) {
                  // We still have a constant offset that is 4-aligned. Push it so we look at its users.
                  dwordOffset += gepByteOffset / 4;
                  users.push_back({gep, dwordOffset});
                  continue;
                }
              }
            }
            // We have found some user we can't handle. Mark that we need to keep the push const pointer.
            userDataUsage->pushConstSpill = true;
          }
        }
      }
      continue;
    }

    if (func.getName().startswith(lgcName::RootDescriptor)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        unsigned dwordOffset = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto &rootDescriptors = getUserDataUsage(stage)->rootDescriptors;
        rootDescriptors.resize(std::max(rootDescriptors.size(), size_t(dwordOffset + 1)));
        rootDescriptors[dwordOffset].users.push_back(call);
      }
      continue;
    }

    if (func.getName().startswith(lgcName::SpecialUserData)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto &specialUserData = getUserDataUsage(stage)->specialUserData;
        unsigned index = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue() -
                         static_cast<unsigned>(UserDataMapping::GlobalTable);
        specialUserData.resize(std::max(specialUserData.size(), size_t(index + 1)));
        specialUserData[index].users.push_back(call);
      }
      continue;
    }

    if (func.getName().startswith(lgcName::DescriptorSet)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        unsigned set = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto &descriptorSets = getUserDataUsage(stage)->descriptorSets;
        descriptorSets.resize(std::max(descriptorSets.size(), size_t(set + 1)));
        descriptorSets[set].users.push_back(call);
      }
    } else if (func.getName().startswith(lgcName::OutputExportXfb) && !func.use_empty()) {
      auto lastVertexStage = m_pipelineState->getLastVertexProcessingStage();
      lastVertexStage = lastVertexStage == ShaderStageCopyShader ? ShaderStageGeometry : lastVertexStage;
      getUserDataUsage(lastVertexStage)->usesStreamOutTable = true;
    }
  }
}

// =====================================================================================================================
// Fix up user data uses in all shaders: For unspilled ones, use the entry arg directly; for spilled ones,
// insert a load from the spill table, shared for the function.
// This uses the entryArgIdx fields in UserDataUsage; each one was set as follows:
// 1. addUserDataArgs constructed a UserDataArg for it, giving it a pointer to the applicable entryArgIdx field;
// 2. In determineUnspilledUserDataArgs, where it decides to unspill (i.e. keep in shader entry SGPR), it stores the
//    argument index into that pointed to value;
// 3. In this function, we use the entryArgIdx field to get the argument index. If it is 0, then the item was
//    spilled.
//
// @param module : IR module
void PatchEntryPointMutate::fixupUserDataUses(Module &module) {
  BuilderBase builder(module.getContext());

  // For each function definition...
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;

    ShaderStage stage = getShaderStage(&func);
    auto userDataUsage = getUserDataUsage(stage);

    // If needed, generate code for the spill table pointer (as pointer to i8) at the start of the function.
    Instruction *spillTable = nullptr;
    AddressExtender addressExtender(&func);
    if (userDataUsage->spillTable.entryArgIdx != 0) {
      builder.SetInsertPoint(addressExtender.getFirstInsertionPt());
      Argument *arg = func.getArg(userDataUsage->spillTable.entryArgIdx);
      arg->setName("spillTable");
      spillTable = addressExtender.extend(arg, builder.getInt32(HighAddrPc),
                                          builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST), builder);
    }

    // Handle direct uses of the spill table that were generated in DescBuilder.
    for (Instruction *&call : userDataUsage->spillTable.users) {
      if (call && call->getFunction() == &func) {
        call->replaceAllUsesWith(spillTable);
        call->eraseFromParent();
        call = nullptr;
      }
    }

    // Handle unspilled parts of the push constant.
    for (unsigned dwordOffset = 0; dwordOffset != userDataUsage->pushConstOffsets.size(); ++dwordOffset) {
      UserDataNodeUsage &pushConstOffset = userDataUsage->pushConstOffsets[dwordOffset];
      if (!pushConstOffset.users.empty()) {
        if (pushConstOffset.entryArgIdx) {
          // This offset into the push constant is unspilled. Replace the loads with the entry arg, with a
          // bitcast. (We know that all loads are non-aggregates of the same size, so we can bitcast.)
          Argument *arg = func.getArg(pushConstOffset.entryArgIdx);
          arg->setName("pushConst_" + Twine(dwordOffset));
          for (Instruction *&load : pushConstOffset.users) {
            if (load && load->getFunction() == &func) {
              builder.SetInsertPoint(load);
              Value *replacement = nullptr;
              if (!isa<PointerType>(load->getType()))
                replacement = builder.CreateBitCast(arg, load->getType());
              else {
                // For a pointer, we need to bitcast to a single int first, then to the pointer.
                replacement = builder.CreateBitCast(arg, builder.getIntNTy(arg->getType()->getPrimitiveSizeInBits()));
                replacement = builder.CreateIntToPtr(replacement, load->getType());
              }
              load->replaceAllUsesWith(replacement);
              load->eraseFromParent();
              load = nullptr;
            }
          }
        } else {
          // This offset into the push constant is spilled. All we need to do is ensure that the push constant
          // pointer (derived as an offset into the spill table) remains.
          userDataUsage->pushConstSpill = true;
        }
      }
    }

    // Handle the push constant pointer.
    if (!userDataUsage->pushConst.users.empty()) {
      // If all uses of the push constant pointer are unspilled, we can just replace the lgc.push.const call
      // with undef, as the address is ultimately not used anywhere.
      Value *replacementVal = nullptr;
      if (userDataUsage->pushConstSpill) {
        // At least one use of the push constant pointer remains.
        const ResourceNode *node = m_pipelineState->findSingleRootResourceNode(ResourceNodeType::PushConst);
        Value *byteOffset = nullptr;
        builder.SetInsertPoint(spillTable->getNextNode());
        if (node) {
          byteOffset = builder.getInt32(node->offsetInDwords * 4);
          // Ensure we mark spill table usage.
          m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
        } else if (!m_pipelineState->isUnlinked()) {
          byteOffset = UndefValue::get(builder.getInt32Ty());
        } else {
          // Unlinked shader compilation: Use a reloc.
          byteOffset = builder.CreateRelocationConstant(reloc::Pushconst);
        }
        replacementVal = builder.CreateGEP(builder.getInt8Ty(), spillTable, byteOffset);
      }
      for (Instruction *&call : userDataUsage->pushConst.users) {
        if (call && call->getFunction() == &func) {
          Value *thisReplacementVal = replacementVal;
          if (!thisReplacementVal) {
            // No use of the push constant pointer remains. Just replace with undef.
            thisReplacementVal = UndefValue::get(call->getType());
          } else {
            builder.SetInsertPoint(call);
            thisReplacementVal = builder.CreateBitCast(thisReplacementVal, call->getType());
          }
          call->replaceAllUsesWith(thisReplacementVal);
          call->eraseFromParent();
          call = nullptr;
        }
      }
    }

    // Root descriptors ("dynamic descriptors").
    for (unsigned dwordOffset = 0; dwordOffset != userDataUsage->rootDescriptors.size(); ++dwordOffset) {
      auto &rootDescriptor = userDataUsage->rootDescriptors[dwordOffset];
      if (rootDescriptor.users.empty())
        continue;
      if (rootDescriptor.entryArgIdx != 0) {
        // The root descriptor is unspilled, and uses an entry arg.
        Argument *arg = func.getArg(rootDescriptor.entryArgIdx);
        arg->setName("rootDesc" + Twine(dwordOffset));
        for (Instruction *&call : rootDescriptor.users) {
          if (call && call->getFunction() == &func) {
            call->replaceAllUsesWith(arg);
            call->eraseFromParent();
            call = nullptr;
          }
        }
      } else {
        // The root descriptor is spilled. Ensure we mark spill table usage.
        m_pipelineState->getPalMetadata()->setUserDataSpillUsage(dwordOffset);
        Value *byteOffset = builder.getInt32(dwordOffset * 4);
        for (Instruction *&call : rootDescriptor.users) {
          if (call && call->getFunction() == &func) {
            builder.SetInsertPoint(call);
            Value *descPtr = builder.CreateGEP(builder.getInt8Ty(), spillTable, byteOffset);
            descPtr = builder.CreateBitCast(descPtr, call->getType()->getPointerTo(ADDR_SPACE_CONST));
            Value *desc = builder.CreateLoad(call->getType(), descPtr);
            desc->setName("rootDesc" + Twine(dwordOffset));
            call->replaceAllUsesWith(desc);
            call->eraseFromParent();
            call = nullptr;
          }
        }
      }
    }

    // Descriptor sets.
    for (unsigned descSetIdx = 0; descSetIdx != userDataUsage->descriptorSets.size(); ++descSetIdx) {
      auto &descriptorSet = userDataUsage->descriptorSets[descSetIdx];
      Instruction *load = nullptr;
      for (Instruction *&inst : descriptorSet.users) {
        Value *descSetVal = nullptr;
        if (inst && inst->getFunction() == &func) {
          auto call = cast<CallInst>(inst);
          Value *highHalf = call->getArgOperand(1);
          if (descriptorSet.entryArgIdx != 0) {
            // The descriptor set is unspilled, and uses an entry arg.
            descSetVal = func.getArg(descriptorSet.entryArgIdx);
            if (isa<ConstantInt>(highHalf)) {
              // Set builder to insert the 32-to-64 extension code at the start of the function.
              builder.SetInsertPoint(addressExtender.getFirstInsertionPt());
            } else {
              // Set builder to insert the 32-to-64 extension code after the instruction containing the high half.
              Instruction *highHalfInst = cast<Instruction>(highHalf);
              builder.SetInsertPoint(highHalfInst->getNextNode());
            }
          } else {
            // The descriptor set is spilled, so we to GEP an offset from the spill table and load from that. For all
            // users, we share a single load at the start of the function.
            if (!load) {
              // This is the first use we have seen in this function. Create the GEP and load, straight after the
              // code at the start of the function that extends the spill table pointer.
              builder.SetInsertPoint(spillTable->getNextNode());
              const ResourceNode *node;
              const ResourceNode *innerNode;
              std::tie(node, innerNode) =
                  m_pipelineState->findResourceNode(ResourceNodeType::DescriptorTableVaPtr, descSetIdx, 0);
              ((void)innerNode);
              Value *offset = nullptr;
              if (node) {
                // We have the user data node for the desciptor set. Use the offset from that.
                m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
                offset = builder.getInt32(node->offsetInDwords * 4);
              } else {
                // Shader compilation. Use a reloc.
                assert(m_pipelineState->isUnlinked());
                offset = builder.CreateRelocationConstant("descset_" + Twine(descSetIdx));
              }
              Value *addr = builder.CreateGEP(builder.getInt8Ty(), spillTable, offset);
              addr = builder.CreateBitCast(addr, builder.getInt32Ty()->getPointerTo(ADDR_SPACE_CONST));
              load = builder.CreateLoad(builder.getInt32Ty(), addr);
            }
            descSetVal = load;
            // Set builder to insert the 32-to-64 extension code just after the load.
            builder.SetInsertPoint(load->getNextNode());
          }
          descSetVal->setName("descSet" + Twine(descSetIdx));

          // Now we want to extend the loaded 32-bit value to a 64-bit pointer, using either PC or the provided
          // high half.
          descSetVal = addressExtender.extend(descSetVal, highHalf, call->getType(), builder);
          // Replace uses of the call and erase it.
          call->replaceAllUsesWith(descSetVal);
          call->eraseFromParent();
          inst = nullptr;
        }
      }
    }

    // Special user data from lgc.special.user.data calls
    for (unsigned idx = 0; idx != userDataUsage->specialUserData.size(); ++idx) {
      auto &specialUserData = userDataUsage->specialUserData[idx];
      if (!specialUserData.users.empty()) {
        Value *arg = nullptr;
        if (specialUserData.entryArgIdx == 0) {
          // This is the case that no arg was created for this value. That can happen, for example when
          // ViewIndex is used but is not enabled in pipeline state. So we need to handle it. We just replace
          // it with UndefValue.
          arg = UndefValue::get(specialUserData.users[0]->getType());
        } else {
          arg = func.getArg(specialUserData.entryArgIdx);
          arg->setName(ShaderInputs::getSpecialUserDataName(static_cast<unsigned>(UserDataMapping::GlobalTable) + idx));
        }
        for (Instruction *&inst : specialUserData.users) {
          if (inst && inst->getFunction() == &func) {
            Value *replacementVal = arg;
            auto call = dyn_cast<CallInst>(inst);
            if (call->getNumArgOperands() >= 2) {
              // There is a second operand, used by ShaderInputs::getSpecialUserDataAsPoint to indicate that we
              // need to extend the loaded 32-bit value to a 64-bit pointer, using either PC or the provided
              // high half.
              builder.SetInsertPoint(call);
              Value *highHalf = call->getArgOperand(1);
              replacementVal = addressExtender.extend(replacementVal, highHalf, call->getType(), builder);
            }
            inst->replaceAllUsesWith(replacementVal);
            inst->eraseFromParent();
            inst = nullptr;
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Process a single shader
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs
void PatchEntryPointMutate::processShader(ShaderInputs *shaderInputs) {
  // Create new entry-point from the original one
  SmallVector<Type *, 8> argTys;
  uint64_t inRegMask = generateEntryPointArgTys(shaderInputs, argTys);

  Function *origEntryPoint = m_entryPoint;

  // Create the new function and transfer code and attributes to it.
  Function *entryPoint =
      addFunctionArgs(origEntryPoint, origEntryPoint->getFunctionType()->getReturnType(), argTys, inRegMask);

  // Set Attributes on new function here.
  AttrBuilder builder;
  if (m_shaderStage == ShaderStageFragment) {
    auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
    SpiPsInputAddr spiPsInputAddr = {};

    spiPsInputAddr.bits.perspSampleEna =
        ((builtInUsage.smooth && builtInUsage.sample) || builtInUsage.baryCoordSmoothSample);
    spiPsInputAddr.bits.perspCenterEna = ((builtInUsage.smooth && builtInUsage.center) || builtInUsage.baryCoordSmooth);
    spiPsInputAddr.bits.perspCentroidEna =
        ((builtInUsage.smooth && builtInUsage.centroid) || builtInUsage.baryCoordSmoothCentroid);
    spiPsInputAddr.bits.perspPullModelEna =
        ((builtInUsage.smooth && builtInUsage.pullMode) || builtInUsage.baryCoordPullModel);
    spiPsInputAddr.bits.linearSampleEna =
        ((builtInUsage.noperspective && builtInUsage.sample) || builtInUsage.baryCoordNoPerspSample);
    spiPsInputAddr.bits.linearCenterEna =
        ((builtInUsage.noperspective && builtInUsage.center) || builtInUsage.baryCoordNoPersp);
    spiPsInputAddr.bits.linearCentroidEna =
        ((builtInUsage.noperspective && builtInUsage.centroid) || builtInUsage.baryCoordNoPerspCentroid);
    spiPsInputAddr.bits.posXFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posYFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posZFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posWFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.frontFaceEna = builtInUsage.frontFacing;
    spiPsInputAddr.bits.ancillaryEna = builtInUsage.sampleId;
    spiPsInputAddr.bits.ancillaryEna |= builtInUsage.shadingRate;
    spiPsInputAddr.bits.sampleCoverageEna = builtInUsage.sampleMaskIn;

    builder.addAttribute("InitialPSInputAddr", std::to_string(spiPsInputAddr.u32All));
  }

  // Set VGPR, SGPR, and wave limits
  auto shaderOptions = &m_pipelineState->getShaderOptions(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  unsigned vgprLimit = shaderOptions->vgprLimit;
  unsigned sgprLimit = shaderOptions->sgprLimit;

  if (vgprLimit != 0) {
    builder.addAttribute("amdgpu-num-vgpr", std::to_string(vgprLimit));
    resUsage->numVgprsAvailable = std::min(vgprLimit, resUsage->numVgprsAvailable);
  }
  resUsage->numVgprsAvailable =
      std::min(resUsage->numVgprsAvailable, m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable);

  if (sgprLimit != 0) {
    builder.addAttribute("amdgpu-num-sgpr", std::to_string(sgprLimit));
    resUsage->numSgprsAvailable = std::min(sgprLimit, resUsage->numSgprsAvailable);
  }
  resUsage->numSgprsAvailable =
      std::min(resUsage->numSgprsAvailable, m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable);

  if (shaderOptions->maxThreadGroupsPerComputeUnit != 0) {
    std::string wavesPerEu = std::string("1,") + std::to_string(shaderOptions->maxThreadGroupsPerComputeUnit);
    builder.addAttribute("amdgpu-waves-per-eu", wavesPerEu);
  }

  if (shaderOptions->unrollThreshold != 0)
    builder.addAttribute("amdgpu-unroll-threshold", std::to_string(shaderOptions->unrollThreshold));
  else {
    // use a default unroll threshold of 700
    builder.addAttribute("amdgpu-unroll-threshold", "700");
  }

  AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
  entryPoint->addAttributes(attribIdx, builder);

  // NOTE: Remove "readnone" attribute for entry-point. If GS is emtry, this attribute will allow
  // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
  if (entryPoint->hasFnAttribute(Attribute::ReadNone))
    entryPoint->removeFnAttr(Attribute::ReadNone);

  // Remove original entry-point
  origEntryPoint->dropAllReferences();
  origEntryPoint->eraseFromParent();
}

// =====================================================================================================================
// Generates the type for the new entry-point based on already-collected info.
// This is what decides what SGPRs and VGPRs are passed to the shader at wave dispatch:
//
// * (For a GFX9+ merged shader or NGG primitive shader, the 8 system SGPRs at the start are not accounted for here.)
// * The "user data" SGPRs, up to 32 (GFX9+ non-compute shader) or 16 (compute shader or <=GFX8). Many of the values
//   here are pointers, but are passed as a single 32-bit register and then expanded to 64-bit in the shader code:
//   - The "global information table", containing various descriptors such as the inter-shader rings
//   - The "per-shader table", which is added here but appears to be unused
//   - The streamout table if needed
//   - Nodes from the root user data layout, including pointers to descriptor sets.
//   - Various other system values set up by PAL, such as the vertex buffer table and the vertex base index
//   - The spill table pointer if needed. This is typically in the last register (s15 or s31), but not necessarily.
// * The system value SGPRs and VGPRs determined by hardware, some of which are enabled or disabled by bits in SPI
//   registers.
//
// In GFX9+ shader merging, shaders have not yet been merged, and this function is called for each
// unmerged shader stage. The code here needs to ensure that it gets the same SGPR user data layout for
// both shaders that are going to be merged (VS-HS, VS-GS if no tessellation, ES-GS).
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs
// @param [out] inRegMask : "Inreg" bit mask for the arguments, with a bit set to indicate that the corresponding
//                          arg needs to have an "inreg" attribute to put the arg into SGPRs rather than VGPRs
// @returns : The newly-constructed function type
//
uint64_t PatchEntryPointMutate::generateEntryPointArgTys(ShaderInputs *shaderInputs, SmallVectorImpl<Type *> &argTys) {

  uint64_t inRegMask = 0;
  IRBuilder<> builder(*m_context);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  entryArgIdxs.initialized = true;

  // First we collect the user data args in two vectors:
  // - userDataArgs: global table, per-shader table and streamout table, followed by the nodes from the root user
  //   data layout (excluding vertex buffer and streamout tables). Some of them may need to be spilled due to
  //   running out of entry SGPRs
  // - specialUserDataArgs: special values that go at the end, such as ViewId.
  //
  // The UserDataArg for each arg pushed into these vectors contains:
  // - argTy: The IR type of the arg
  // - argDwordSize: Size of the arg in dwords
  // - userDataValue: The PAL metadata value to be passed to PalMetadata::setUserDataEntry, or Invalid for none
  // - argIndex: Pointer to the location where we will store the actual arg number, or nullptr

  SmallVector<UserDataArg, 8> userDataArgs;
  SmallVector<UserDataArg, 4> specialUserDataArgs;

  // Global internal table
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), UserDataMapping::GlobalTable));

  // Per-shader table
  // TODO: We need add per shader table per real usage after switch to PAL new interface.
  // if (pResUsage->perShaderTable)
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty()));

  addSpecialUserDataArgs(userDataArgs, specialUserDataArgs, builder);

  addUserDataArgs(userDataArgs, builder);

  // Determine which user data args are going to be "unspilled", and put them in unspilledArgs.
  SmallVector<UserDataArg, 8> unspilledArgs;
  determineUnspilledUserDataArgs(userDataArgs, specialUserDataArgs, builder, unspilledArgs);

  // Scan unspilledArgs: for each one:
  // * add it to the arg type array
  // * set user data PAL metadata
  // * store the arg index into the pointer provided to the xxxArgs.push()
  // * if it's special user data, also store the arg index into the specialUserData entry.
  unsigned userDataIdx = 0;
  for (const auto &userDataArg : unspilledArgs) {
    if (userDataArg.argIndex)
      *userDataArg.argIndex = argTys.size();
    unsigned dwordSize = userDataArg.argDwordSize;
    if (userDataArg.userDataValue != static_cast<unsigned>(UserDataMapping::Invalid)) {
      m_pipelineState->getPalMetadata()->setUserDataEntry(m_shaderStage, userDataIdx, userDataArg.userDataValue,
                                                          dwordSize);
      if (userDataArg.userDataValue >= static_cast<unsigned>(UserDataMapping::GlobalTable)) {
        unsigned index = userDataArg.userDataValue - static_cast<unsigned>(UserDataMapping::GlobalTable);
        auto &specialUserData = getUserDataUsage(m_shaderStage)->specialUserData;
        if (index < specialUserData.size())
          specialUserData[index].entryArgIdx = argTys.size();
      }
    }
    argTys.push_back(userDataArg.argTy);
    userDataIdx += dwordSize;
  }

  intfData->userDataCount = userDataIdx;
  inRegMask = (1ull << argTys.size()) - 1;

  // Push the fixed system (not user data) register args.
  inRegMask |= shaderInputs->getShaderArgTys(m_pipelineState, m_shaderStage, argTys);

  return inRegMask;
}

// =====================================================================================================================
// Add a UserDataArg to the appropriate vector for each special argument (e.g. ViewId) needed in user data SGPRs.
// In here, we need to check whether an argument is needed in two ways:
// 1. Whether a flag is set saying it will be needed after PatchEntryPointMutate
// 2. Whether there is an actual use of the special user data value (lgc.special.user.data call) generated
//    before PatchEntryPointMutate, which we check with userDataUsage->isSpecialUserDataUsed().
//
// @param userDataArgs : Vector to add args to when they need to go before user data nodes (just streamout)
// @param specialUserDataArgs : Vector to add args to when they need to go after user data nodes (all the rest)
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                                   SmallVectorImpl<UserDataArg> &specialUserDataArgs,
                                                   IRBuilder<> &builder) {

  auto userDataUsage = getUserDataUsage(m_shaderStage);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  auto &builtInUsage = resUsage->builtInUsage;
  bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessControl ||
      m_shaderStage == ShaderStageTessEval || m_shaderStage == ShaderStageGeometry) {
    // Shader stage in the vertex-processing half of a graphics pipeline.
    // We need to ensure that the layout is the same between two shader stages that will be merged on GFX9+,
    // that is, VS-TCS, VS-GS (if no tessellation), TES-GS.

    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data. We do not add it to PAL metadata
    // for TCS.
    if (m_pipelineState->getInputAssemblyState().enableMultiView) {
      unsigned *argIdx = nullptr;
      auto userDataValue = UserDataMapping::ViewId;
      switch (m_shaderStage) {
      case ShaderStageVertex:
        argIdx = &entryArgIdxs.vs.viewIndex;
        break;
      case ShaderStageTessControl:
        userDataValue = UserDataMapping::Invalid;
        break;
      case ShaderStageTessEval:
        argIdx = &entryArgIdxs.tes.viewIndex;
        break;
      case ShaderStageGeometry:
        argIdx = &entryArgIdxs.gs.viewIndex;
        break;
      default:
        llvm_unreachable("Unexpected shader stage");
      }
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), userDataValue, argIdx));
    }

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (VS is in NGG primitive shader).
    bool wantEsGsLdsSize = false;
    switch (getMergedShaderStage(m_shaderStage)) {
    case ShaderStageVertex:
      wantEsGsLdsSize = enableNgg;
      break;
    case ShaderStageTessControl:
      wantEsGsLdsSize = false;
      break;
    case ShaderStageTessEval:
      wantEsGsLdsSize = enableNgg;
      break;
    case ShaderStageGeometry:
      wantEsGsLdsSize = (m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize) || enableNgg;
      break;
    default:
      llvm_unreachable("Unexpected shader stage");
    }
    if (wantEsGsLdsSize) {
      auto userDataValue = UserDataMapping::EsGsLdsSize;
      // For a standalone TCS (which can only happen in unit testing, not in a real pipeline), don't add
      // the PAL metadata for it, for consistency with the old code.
      if (m_shaderStage == ShaderStageVertex && !m_pipelineState->hasShaderStage(ShaderStageVertex))
        userDataValue = UserDataMapping::Invalid;
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), userDataValue));
    }

    if (getMergedShaderStage(m_shaderStage) == getMergedShaderStage(ShaderStageVertex)) {
      // This is the VS, or the shader that VS is merged into on GFX9+.
      auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);

      // Detect whether this is an unlinked compile that will need a fetch shader. If so, we need to
      // add the vertex buffer table and base vertex and base instance, even if they appear unused here.
      bool willHaveFetchShader = m_pipelineState->getPalMetadata()->getVertexFetchCount() != 0;

      // Vertex buffer table.
      if (willHaveFetchShader || userDataUsage->isSpecialUserDataUsed(UserDataMapping::VertexBufferTable)) {
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), UserDataMapping::VertexBufferTable,
                                                  &vsIntfData->entryArgIdxs.vs.vbTablePtr));
      }

      // Base vertex and base instance.
      if (willHaveFetchShader || vsResUsage->builtInUsage.vs.baseVertex || vsResUsage->builtInUsage.vs.baseInstance ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseVertex) ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseInstance)) {
        specialUserDataArgs.push_back(
            UserDataArg(builder.getInt32Ty(), UserDataMapping::BaseVertex, &vsIntfData->entryArgIdxs.vs.baseVertex));
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), UserDataMapping::BaseInstance,
                                                  &vsIntfData->entryArgIdxs.vs.baseInstance));
      }

      // Draw index.
      if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::DrawIndex))
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), UserDataMapping::DrawIndex));
    }

  } else if (m_shaderStage == ShaderStageCompute) {
    // Emulate gl_NumWorkGroups via user data registers.
    // Later code needs to ensure that this starts on an even numbered register.
    if (builtInUsage.cs.numWorkgroups || userDataUsage->isSpecialUserDataUsed(UserDataMapping::Workgroup)) {
      auto numWorkgroupsPtrTy = PointerType::get(FixedVectorType::get(builder.getInt32Ty(), 3), ADDR_SPACE_CONST);
      specialUserDataArgs.push_back(
          UserDataArg(numWorkgroupsPtrTy, UserDataMapping::Workgroup, &entryArgIdxs.cs.numWorkgroupsPtr));
    }
  }

  // Allocate register for stream-out buffer table, to go before the user data node args (unlike all the ones
  // above, which go after the user data node args).
  if (userDataUsage->usesStreamOutTable || userDataUsage->isSpecialUserDataUsed(UserDataMapping::StreamOutTable)) {
    if (enableNgg || !m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader)->inOutUsage.enableXfb) {
      // If no NGG, stream out table will be set to copy shader's user data entry, we should not set it duplicately.
      auto userDataValue = UserDataMapping::StreamOutTable;
      switch (m_shaderStage) {
      case ShaderStageVertex:
        userDataArgs.push_back(
            UserDataArg(builder.getInt32Ty(), userDataValue, &intfData->entryArgIdxs.vs.streamOutData.tablePtr));
        break;
      case ShaderStageTessEval:
        userDataArgs.push_back(
            UserDataArg(builder.getInt32Ty(), userDataValue, &intfData->entryArgIdxs.tes.streamOutData.tablePtr));
        break;
      // Allocate dummy stream-out register for Geometry shader
      case ShaderStageGeometry:
        userDataArgs.push_back(UserDataArg(builder.getInt32Ty()));
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    }
  }
}

// =====================================================================================================================
// Add a UserDataArg to the vector for each user data node needed in user data SGPRs.
//
// @param userDataArgs : Vector to add args to
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::addUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs, IRBuilder<> &builder) {

  auto userDataUsage = getUserDataUsage(m_shaderStage);
  if (m_pipelineState->isUnlinked() && m_pipelineState->getUserDataNodes().empty()) {
    // Shader compilation with no user data layout. Add descriptor sets directly from the user data usage
    // gathered at the start of this pass.
    for (unsigned descSetIdx = 0; descSetIdx != userDataUsage->descriptorSets.size(); ++descSetIdx) {
      auto &descriptorSet = userDataUsage->descriptorSets[descSetIdx];
      if (!descriptorSet.users.empty()) {
        // Set the PAL metadata user data value to indicate that it needs modifying at link time.
        assert(descSetIdx <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax) -
                                 static_cast<unsigned>(UserDataMapping::DescriptorSet0));
        unsigned userDataValue = static_cast<unsigned>(UserDataMapping::DescriptorSet0) + descSetIdx;
        userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), userDataValue, &descriptorSet.entryArgIdx));
      }
    }

    // Add push constants (if used).
    // We add a potential unspilled arg for each separate dword offset of the push const at which there is a load.
    // We already know that loads we have on our pushConstOffsets lists are at dword-aligned offset and dword-aligned
    // size. We need to ensure that all loads are the same size, by removing ones that are bigger than the
    // minimum size.
    for (unsigned dwordOffset = 0, dwordEndOffset = userDataUsage->pushConstOffsets.size();
         dwordOffset != dwordEndOffset; ++dwordOffset) {
      UserDataNodeUsage &pushConstOffset = userDataUsage->pushConstOffsets[dwordOffset];
      if (pushConstOffset.users.empty())
        continue;

      // Check that the load size does not overlap with the next used offset in the push constant.
      bool haveOverlap = false;
      unsigned endOffset =
          std::min(dwordOffset + pushConstOffset.dwordSize, unsigned(userDataUsage->pushConstOffsets.size()));
      for (unsigned followingOffset = dwordOffset + 1; followingOffset != endOffset; ++followingOffset) {
        if (!userDataUsage->pushConstOffsets[followingOffset].users.empty()) {
          haveOverlap = true;
          break;
        }
      }
      if (haveOverlap) {
        userDataUsage->pushConstSpill = true;
        continue;
      }

      // Add the arg (part of the push const) that we can potentially unspill.
      assert(dwordOffset + pushConstOffset.dwordSize - 1 <=
             static_cast<unsigned>(UserDataMapping::PushConstMax) - static_cast<unsigned>(UserDataMapping::PushConst0));
      addUserDataArg(userDataArgs, static_cast<unsigned>(UserDataMapping::PushConst0) + dwordOffset,
                     pushConstOffset.dwordSize, &pushConstOffset.entryArgIdx, builder);
    }

    return;
  }

  // We do have user data layout.
  // Add entries from the root user data layout (not vertex buffer or streamout, and not unused ones).

  for (unsigned i = 0; i != m_pipelineState->getUserDataNodes().size(); ++i) {
    const ResourceNode &node = m_pipelineState->getUserDataNodes()[i];
    switch (node.type) {

    case ResourceNodeType::IndirectUserDataVaPtr:
    case ResourceNodeType::StreamOutTableVaPtr:
      break;

    case ResourceNodeType::DescriptorTableVaPtr: {
      // Check if the descriptor set is in use.
      if (userDataUsage->descriptorSets.size() <= node.innerTable[0].set)
        break;
      auto &descSetUsage = userDataUsage->descriptorSets[node.innerTable[0].set];
      if (descSetUsage.users.empty())
        break;

      unsigned userDataValue = node.offsetInDwords;
      if (m_pipelineState->getShaderOptions(m_shaderStage).updateDescInElf && m_shaderStage == ShaderStageFragment) {
        // Put set number to register first, will update offset after merge ELFs
        // For partial pipeline compile, only fragment shader needs to adjust offset of root descriptor.
        // This is part of the original "partial pipeline compile" scheme, and it uses a magic number for the
        // PAL metadata register value because the code to fix it up in llpcElfWriter.cpp just fixes up any
        // register with the magic value, and hopes it lucks out by not getting a false positive.
        // TODO: Remove all that code once the new "shader/half-pipeline compile" scheme can replace it.
        static const unsigned DescRelocMagic = 0xA5A5A500;
        userDataValue = DescRelocMagic | node.innerTable[0].set;
      }
      // Add the arg (descriptor set pointer) that we can potentially unspill.
      addUserDataArg(userDataArgs, userDataValue, node.sizeInDwords, &descSetUsage.entryArgIdx, builder);
      break;
    }

    case ResourceNodeType::PushConst: {
      // We add a potential unspilled arg for each separate dword offset of the push const at which there is a load.
      // We already know that loads we have on our pushConstOffsets lists are at dword-aligned offset and dword-aligned
      // size. We need to ensure that all loads are the same size, by removing ones that are bigger than the
      // minimum size.
      //
      // First cope with the case that the app uses more push const than the size of the resource node. This is
      // a workaround for an incorrect application; according to the Vulkan spec (version 1.2.151, section 14.6.1
      // "Push Constant Interface"):
      //
      //    Each statically used member of a push constant block must be placed at an Offset such that the entire
      //    member is entirely contained within the VkPushConstantRange for each OpEntryPoint that uses it, and
      //    the stageFlags for that range must specify the appropriate VkShaderStageFlagBits for that stage.
      unsigned dwordEndOffset = userDataUsage->pushConstOffsets.size();
      if (dwordEndOffset > node.sizeInDwords) {
        userDataUsage->pushConstSpill = true;
        dwordEndOffset = node.sizeInDwords;
      }

      for (unsigned dwordOffset = 0; dwordOffset != dwordEndOffset; ++dwordOffset) {
        UserDataNodeUsage &pushConstOffset = userDataUsage->pushConstOffsets[dwordOffset];
        if (pushConstOffset.users.empty())
          continue;

        // Check that the load size does not overlap with the next used offset in the push constant.
        bool haveOverlap = false;
        unsigned endOffset =
            std::min(dwordOffset + pushConstOffset.dwordSize, unsigned(userDataUsage->pushConstOffsets.size()));
        for (unsigned followingOffset = dwordOffset + 1; followingOffset != endOffset; ++followingOffset) {
          if (!userDataUsage->pushConstOffsets[followingOffset].users.empty()) {
            haveOverlap = true;
            break;
          }
        }
        if (haveOverlap) {
          userDataUsage->pushConstSpill = true;
          continue;
        }

        // Add the arg (part of the push const) that we can potentially unspill.
        addUserDataArg(userDataArgs, node.offsetInDwords + dwordOffset, pushConstOffset.dwordSize,
                       &pushConstOffset.entryArgIdx, builder);
      }

      // Ensure we mark the push constant's part of the spill table as used.
      if (userDataUsage->pushConstSpill)
        userDataUsage->spillUsage = std::min(userDataUsage->spillUsage, node.offsetInDwords);

      break;
    }

    default:
      for (unsigned dwordOffset = node.offsetInDwords; dwordOffset != node.offsetInDwords + node.sizeInDwords;
           ++dwordOffset) {
        if (userDataUsage->rootDescriptors.size() <= dwordOffset)
          break;
        auto &rootDescUsage = userDataUsage->rootDescriptors[dwordOffset];
        if (rootDescUsage.users.empty())
          continue;
        unsigned dwordSize = rootDescUsage.users[0]->getType()->getPrimitiveSizeInBits() / 32;
        // Add the arg (root descriptor) that we can potentially unspill.
        addUserDataArg(userDataArgs, dwordOffset, dwordSize, &rootDescUsage.entryArgIdx, builder);
      }
      break;
    }
  }
}

// =====================================================================================================================
// Add a single UserDataArg
//
// @param userDataArgs : Vector to add UserDataArg to
// @param userDataValue : PAL metadata user data value, ~0U (UserDataMapping::Invalid) for none
// @param sizeInDwords : Size of argument in dwords
// @param argIndex : Where to store arg index once it is allocated, nullptr for none
// @param builder : IRBuilder (just for getting types)
void PatchEntryPointMutate::addUserDataArg(SmallVectorImpl<UserDataArg> &userDataArgs, unsigned userDataValue,
                                           unsigned sizeInDwords, unsigned *argIndex, IRBuilder<> &builder) {
  Type *argTy = builder.getInt32Ty();
  if (sizeInDwords != 1)
    argTy = FixedVectorType::get(argTy, sizeInDwords);
  userDataArgs.push_back(UserDataArg(argTy, userDataValue, argIndex));
}

// =====================================================================================================================
// Determine which user data args are going to be "unspilled" (passed in shader entry SGPRs rather than loaded
// from spill table)
//
// @param userDataArgs : First array of UserDataArg structs for candidate args
// @param specialUserDataArgs : Second array of UserDataArg structs for candidate args
// @param builder : IRBuilder to get types from
// @param [out] unspilledArgs : Output vector of UserDataArg structs that will be "unspilled". Mostly these are
//                              copied from the input arrays, plus an extra one for the spill table pointer if
//                              needed.
void PatchEntryPointMutate::determineUnspilledUserDataArgs(ArrayRef<UserDataArg> userDataArgs,
                                                           ArrayRef<UserDataArg> specialUserDataArgs,
                                                           IRBuilder<> &builder,
                                                           SmallVectorImpl<UserDataArg> &unspilledArgs) {

  SmallVector<UserDataArg, 1> spillTableArg;

  auto userDataUsage = getUserDataUsage(m_shaderStage);
  if (!userDataUsage->spillTable.users.empty() || userDataUsage->pushConstSpill ||
      userDataUsage->spillUsage != UINT_MAX) {
    // Spill table is already in use by code added in DescBuilder, or by uses of the push const pointer not
    // all being of the form that can be unspilled.
    spillTableArg.push_back(
        UserDataArg(builder.getInt32Ty(), UserDataMapping::SpillTable, &userDataUsage->spillTable.entryArgIdx));

    // Determine the lowest offset at which the spill table is used, so we can set PAL metadata accordingly.
    // (This only covers uses of the spill table generated by DescBuilder. It excludes the push const and args
    // that are unspill candidates but we decide to spill; those ones are separately set in userDataUsage->spillUsage.)
    SmallVector<Instruction *, 4> spillUsers;
    spillUsers.insert(spillUsers.end(), userDataUsage->spillTable.users.begin(), userDataUsage->spillTable.users.end());
    unsigned minByteOffset = UINT_MAX;
    for (unsigned i = 0; i != spillUsers.size(); ++i) {
      for (User *user : spillUsers[i]->users()) {
        auto inst = cast<Instruction>(user);
        if (isa<BitCastInst>(inst)) {
          spillUsers.push_back(inst);
          continue;
        }
        if (auto gep = dyn_cast<GetElementPtrInst>(inst)) {
          APInt gepOffset(64, 0);
          if (gep->accumulateConstantOffset(m_module->getDataLayout(), gepOffset)) {
            minByteOffset = std::min(minByteOffset, unsigned(gepOffset.getZExtValue()));
            continue;
          }
        }
        minByteOffset = 0;
        break;
      }
    }
    // In relocatable shader compilation userDataUsage is unknown until linking.
    if (minByteOffset != UINT_MAX && !m_pipelineState->isUnlinked())
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(std::min(userDataUsage->spillUsage, minByteOffset / 4));
  }

  // Figure out how many sgprs we have available for userDataArgs.
  unsigned userDataEnd = 0;
  // We have s0-s31 (s0-s15 for <=GFX8, or for a compute shader on any chip) for everything, so take off the number
  // of registers used by specialUserDataArgs.
  userDataEnd = m_shaderStage == ShaderStageCompute
                    ? InterfaceData::MaxCsUserDataCount
                    : m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;
  for (auto &userDataArg : specialUserDataArgs)
    userDataEnd -= userDataArg.argDwordSize;
  // ... and the one used by the spill table if already added.
  userDataEnd -= spillTableArg.size();

  // See if we need to spill any user data nodes in userDataArgs, copying the unspilled ones across to unspilledArgs.
  unsigned userDataIdx = 0;

  for (const UserDataArg &userDataArg : userDataArgs) {
    unsigned afterUserDataIdx = userDataIdx + userDataArg.argDwordSize;
    if (afterUserDataIdx > userDataEnd) {
      // Spill this node. Allocate the spill table arg.
      if (spillTableArg.empty()) {
        spillTableArg.push_back(
            UserDataArg(builder.getInt32Ty(), UserDataMapping::SpillTable, &userDataUsage->spillTable.entryArgIdx));
        --userDataEnd;

        if (userDataIdx > userDataEnd) {
          // We over-ran the available SGPRs by filling them up and then realizing we needed a spill table pointer.
          // Remove the last unspilled node (and any padding arg before that), and ensure that spill usage is
          // set correctly so that PAL metadata spill threshold is correct.
          userDataIdx -= unspilledArgs.back().argDwordSize;
          userDataUsage->spillUsage = std::min(userDataUsage->spillUsage, unspilledArgs.back().userDataValue);
          unspilledArgs.pop_back();
        }
      }
      // Ensure that spillUsage includes this offset. (We might be on a compute shader padding node, in which
      // case userDataArg.userDataValue is Invalid, and this call has no effect.)
      userDataUsage->spillUsage = std::min(userDataUsage->spillUsage, userDataArg.userDataValue);

      continue;
    }
    // Keep this node on the unspilled list.
    userDataIdx = afterUserDataIdx;
    unspilledArgs.push_back(userDataArg);
  }

  // Add the special args and the spill table pointer (if any) to unspilledArgs.
  unspilledArgs.insert(unspilledArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
  unspilledArgs.insert(unspilledArgs.end(), spillTableArg.begin(), spillTableArg.end());
}

// =====================================================================================================================
// Get UserDataUsage struct for the merged shader stage that contains the given shader stage
//
// @param stage : Shader stage
PatchEntryPointMutate::UserDataUsage *PatchEntryPointMutate::getUserDataUsage(ShaderStage stage) {
  stage = getMergedShaderStage(stage);
  m_userDataUsage.resize(std::max(m_userDataUsage.size(), static_cast<size_t>(stage) + 1));
  if (!m_userDataUsage[stage])
    m_userDataUsage[stage] = std::make_unique<UserDataUsage>();
  return &*m_userDataUsage[stage];
}

// =====================================================================================================================
// Get the shader stage that the given shader stage is merged into.
// For GFX9+:
// VS -> TCS (if it exists)
// VS -> GS (if it exists)
// TES -> GS (if it exists)
//
// @param stage : Shader stage
ShaderStage PatchEntryPointMutate::getMergedShaderStage(ShaderStage stage) const {
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
    switch (stage) {
    case ShaderStageVertex:
      if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
        return ShaderStageTessControl;
      LLVM_FALLTHROUGH;
    case ShaderStageTessEval:
      if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
        return ShaderStageGeometry;
      break;
    default:
      break;
    }
  }
  return stage;
}

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for entry-point mutation.
INITIALIZE_PASS(PatchEntryPointMutate, DEBUG_TYPE, "Patch LLVM for entry-point mutation", false, false)
