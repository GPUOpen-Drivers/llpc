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
 * @file  PatchEntryPointMutate.cpp
 * @brief The lgc::PatchEntryPointMutate pass determines the final user data layout of shaders.
 *
 * This consists of
 * - removing unused user data
 * - unspilling root descriptors if possible (moving from spill table into user data registers)
 * - unspilling push constants if we never need a pointer to them
 * - putting push constants into registers if no code needs a pointer to it
 * - figuring out where to put user data.
 *
 * The final user data is written into a limited number of sgprs starting with s0. If the user data does not fit in
 * there completely, the last i32 is changed to be a pointer to a spill table in memory, that contains the rest of the
 * user data.
 *
 * Root descriptors are dynamic uniform buffer descriptors in Vulkan, that can be changed without modifying a descriptor
 * set and rebuilding the pipeline. They get put into the spill table but can be unspilled.
 *
 * Special care is required for compute libraries. Similar to unlinked shader compilation, we do not know the final
 * layout for non-entrypoint shaders. For compute libraries, user data args must be passed to other functions, whose
 * implementation is unknown at compile time. Therefore, computation of user data arguments must be independent of any
 * instructions or uses. This is important, even for functions that have no calls, as we still need to compute the taken
 * arguments in a deterministic layout. For library functions, only a prefix of the user data is known at compile time.
 * There can be more user data at runtime, and that needs to be passed on to called functions. Therefore, we
 * - always pass all possible user data registers, even if they have no content for the current shader
 * - have a spill table pointer in the largest user data sgpr
 * - cannot remove unused user data as it might be used by a callee.
 ***********************************************************************************************************************
 */

#include "lgc/patch/PatchEntryPointMutate.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h" // for MemoryEffects
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

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

// =====================================================================================================================
PatchEntryPointMutate::PatchEntryPointMutate() : m_hasTs(false), m_hasGs(false) {
}

// =====================================================================================================================
PatchEntryPointMutate::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name, unsigned userDataValue,
                                                unsigned *argIndex)
    : argTy(argTy), name(name.str()), userDataValue(userDataValue), argIndex(argIndex) {
  if (llvm::isa<llvm::PointerType>(argTy))
    argDwordSize = argTy->getPointerAddressSpace() == ADDR_SPACE_CONST_32BIT ? 1 : 2;
  else
    argDwordSize = argTy->getPrimitiveSizeInBits() / 32;
}

// =====================================================================================================================
PatchEntryPointMutate::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name,
                                                UserDataMapping userDataValue, unsigned *argIndex)
    : UserDataArg(argTy, name, static_cast<unsigned>(userDataValue), argIndex) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchEntryPointMutate::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  runImpl(module, pipelineShaders, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchEntryPointMutate::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                    PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Entry-Point-Mutate\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  // Gather user data usage.
  gatherUserDataUsage(&module);

  // Create ShaderInputs object and gather shader input usage.
  ShaderInputs shaderInputs;
  shaderInputs.gatherUsage(module);
  setupComputeWithCalls(&module);

  if (m_pipelineState->isGraphics()) {
    // Process each shader in turn, but not the copy shader.
    for (unsigned shaderStage = 0; shaderStage < ShaderStageNativeStageCount; ++shaderStage) {
      m_entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));
      if (m_entryPoint) {
        m_shaderStage = static_cast<ShaderStage>(shaderStage);
        processShader(&shaderInputs);
      }
    }
  } else {
    processComputeFuncs(&shaderInputs, module);
  }

  // Fix up user data uses to use entry args.
  fixupUserDataUses(*m_module);
  m_userDataUsage.clear();

  // Fix up shader input uses to use entry args.
  shaderInputs.fixupUses(*m_module, m_pipelineState);

  return true;
}

// =====================================================================================================================
// Set up compute-with-calls flag. It is set for either of these two cases:
// 1. a compute library;
// 2. a compute pipeline that does indirect calls or calls to external functions.
//
// When set, this pass behaves differently, not attempting to omit unused shader inputs, since all shader inputs
// are potentially used in other functions. It also modifies each call to pass the shader inputs between functions.
//
// @param module : IR module
void PatchEntryPointMutate::setupComputeWithCalls(Module *module) {
  m_computeWithCalls = false;

  if (m_pipelineState->isComputeLibrary()) {
    m_computeWithCalls = true;
    return;
  }

  // We have a compute pipeline. Check whether there are any non-shader-entry-point functions (other than lgc.*
  // functions and intrinsics).
  for (Function &func : *module) {
    if (func.isDeclaration() && func.getIntrinsicID() == Intrinsic::not_intrinsic &&
        !func.getName().startswith(lgcName::InternalCallPrefix) && !func.user_empty()) {
      m_computeWithCalls = true;
      return;
    }

    // Search for indirect calls
    for (const BasicBlock &block : func) {
      for (const Instruction &inst : block) {
        if (auto *call = dyn_cast<CallInst>(&inst)) {
          Value *calledVal = call->getCalledOperand();
          if (isa<Function>(calledVal) || call->isInlineAsm())
            continue;
          m_computeWithCalls = true;
          return;
        }
      }
    }
  }
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

    if (func.getName().startswith(lgcName::DescriptorTableAddr)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        ResourceNodeType resType = ResourceNodeType(cast<ConstantInt>(call->getArgOperand(0))->getZExtValue());
        ResourceNodeType searchType = ResourceNodeType(cast<ConstantInt>(call->getArgOperand(1))->getZExtValue());
        unsigned set = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
        unsigned binding = cast<ConstantInt>(call->getArgOperand(3))->getZExtValue();
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto &descriptorTable = getUserDataUsage(stage)->descriptorTables;

        if (m_pipelineState->isUnlinked() && m_pipelineState->getUserDataNodes().empty()) {
          if (m_pipelineState->getOptions().resourceLayoutScheme == ResourceLayoutScheme::Indirect) {
            // If the type is pushconst, the index is set 0, others are set + 1.
            if (resType == ResourceNodeType::PushConst) {
              descriptorTable.resize(std::max(descriptorTable.size(), size_t(1)));
              descriptorTable[0].users.push_back(call);
            } else {
              descriptorTable.resize(std::max(descriptorTable.size(), size_t(set + 2)));
              descriptorTable[set + 1].users.push_back(call);
            }
          } else {
            // The user data nodes are not available, so we use the set as the index.
            descriptorTable.resize(std::max(descriptorTable.size(), size_t(set + 1)));
            descriptorTable[set].users.push_back(call);
          }
        } else {
          // The user data nodes are available, so we use the offset of the node as the
          // index.
          const ResourceNode *node;
          node = m_pipelineState->findResourceNode(searchType, set, binding).first;
          if (!node) {
            // Handle mutable descriptors
            node = m_pipelineState->findResourceNode(ResourceNodeType::DescriptorMutable, set, binding).first;
          }
          assert(node && "Could not find resource node");
          uint32_t descTableIndex = node - &m_pipelineState->getUserDataNodes().front();
          descriptorTable.resize(std::max(descriptorTable.size(), size_t(descTableIndex + 1)));
          descriptorTable[descTableIndex].users.push_back(call);
        }
      }
    } else if ((func.getName().startswith(lgcName::OutputExportXfb) && !func.use_empty()) ||
               m_pipelineState->enableSwXfb()) {
      // NOTE: For GFX11+, SW emulated stream-out will always use stream-out buffer descriptors and stream-out buffer
      // offsets to calculate numbers of written primitives/dwords and update the counters.  auto lastVertexStage =
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

    // Only entrypoint and amd_gfx functions use user data, others don't use.
    if (!isShaderEntryPoint(&func) && (func.getCallingConv() != CallingConv::AMDGPU_Gfx))
      continue;

    ShaderStage stage = getShaderStage(&func);
    auto userDataUsage = getUserDataUsage(stage);

    // If needed, generate code for the spill table pointer (as pointer to i8) at the start of the function.
    Instruction *spillTable = nullptr;
    AddressExtender addressExtender(&func);
    if (userDataUsage->spillTable.entryArgIdx != 0) {
      builder.SetInsertPoint(addressExtender.getFirstInsertionPt());
      Argument *arg = getFunctionArgument(&func, userDataUsage->spillTable.entryArgIdx);
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
          Argument *arg = getFunctionArgument(&func, pushConstOffset.entryArgIdx);
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

    // Handle the push constant pointer, always do that for compute libraries.
    if (!userDataUsage->pushConst.users.empty() || isComputeWithCalls()) {
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
        Argument *arg = getFunctionArgument(&func, rootDescriptor.entryArgIdx);
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

    // Descriptor tables
    Type *ptrType = builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST);
    for (unsigned userDataIdx = 0; userDataIdx != userDataUsage->descriptorTables.size(); ++userDataIdx) {
      auto &descriptorTable = userDataUsage->descriptorTables[userDataIdx];
      Instruction *spillTableLoad = nullptr;
      const bool isDescTableSpilled = descriptorTable.entryArgIdx == 0;

      SmallDenseMap<Value *, Value *> addrExtMap[2];
      for (Instruction *&inst : descriptorTable.users) {
        Value *descTableVal = nullptr;
        if (inst && inst->getFunction() == &func) {
          auto call = cast<CallInst>(inst);
          assert(call->getType() == ptrType);

          if (isDescTableSpilled && !spillTableLoad) {
            // The descriptor table is spilled. At the start of the function, create the GEP and load which are then
            // shared by all users.
            std::string namePrefix = "descTable";
            builder.SetInsertPoint(spillTable->getNextNode());
            Value *offset = nullptr;
            if (!m_pipelineState->isUnlinked() || !m_pipelineState->getUserDataNodes().empty()) {
              const ResourceNode *node = &m_pipelineState->getUserDataNodes()[userDataIdx];
              m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
              offset = builder.getInt32(node->offsetInDwords * 4);
            } else {
              // Shader compilation. Use a relocation to get the descriptor
              // table offset for the descriptor set userDataIdx.
              offset = builder.CreateRelocationConstant(reloc::DescriptorTableOffset + Twine(userDataIdx));
              namePrefix = "descSet";
            }
            Value *addr = builder.CreateGEP(builder.getInt8Ty(), spillTable, offset);
            addr = builder.CreateBitCast(addr, builder.getInt32Ty()->getPointerTo(ADDR_SPACE_CONST));
            spillTableLoad = builder.CreateLoad(builder.getInt32Ty(), addr);
            spillTableLoad->setName(namePrefix + Twine(userDataIdx));
          }

          // The address extension code only depends on descriptorTable (which is constant for the lifetime of the map)
          // and highHalf. Use map with highHalf keys to avoid creating redundant nodes for the extensions.
          Value *highHalf = call->getArgOperand(4);
          auto it = addrExtMap[isDescTableSpilled].find(highHalf);
          if (it != addrExtMap[isDescTableSpilled].end()) {
            descTableVal = it->second;
          } else {

            if (!isDescTableSpilled) {
              // The descriptor set is unspilled, and uses an entry arg.
              descTableVal = getFunctionArgument(&func, descriptorTable.entryArgIdx);
              if (isa<ConstantInt>(highHalf)) {
                // Set builder to insert the 32-to-64 extension code at the start of the function.
                builder.SetInsertPoint(addressExtender.getFirstInsertionPt());
              } else {
                // Set builder to insert the 32-to-64 extension code after the instruction containing the high half.
                Instruction *highHalfInst = cast<Instruction>(highHalf);
                builder.SetInsertPoint(highHalfInst->getNextNode());
              }
            } else {
              // The descriptor table is spilled, the load at the start of the function has been created.
              assert(descriptorTable.entryArgIdx == 0);
              assert(spillTableLoad);
              descTableVal = spillTableLoad;
              // Set builder to insert the 32-to-64 extension code just after the load.
              builder.SetInsertPoint(spillTableLoad->getNextNode());
            }

            // Now we want to extend the loaded 32-bit value to a 64-bit pointer, using either PC or the provided
            // high half.
            descTableVal = addressExtender.extend(descTableVal, highHalf, ptrType, builder);
            addrExtMap[isDescTableSpilled].insert({highHalf, descTableVal});
          }

          // Replace uses of the call and erase it.
          call->replaceAllUsesWith(descTableVal);
          call->eraseFromParent();
          inst = nullptr;
        }
      }
    }

    // Special user data from lgc.special.user.data calls
    for (unsigned idx = 0; idx != userDataUsage->specialUserData.size(); ++idx) {
      auto &specialUserData = userDataUsage->specialUserData[idx];
      if (!specialUserData.users.empty()) {
        assert(specialUserData.entryArgIdx != 0);
        Value *arg = getFunctionArgument(&func, specialUserData.entryArgIdx);

        for (Instruction *&inst : specialUserData.users) {
          if (inst && inst->getFunction() == &func) {
            Value *replacementVal = arg;
            auto call = dyn_cast<CallInst>(inst);
            if (call->arg_size() >= 2) {
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
  SmallVector<std::string, 8> argNames;
  uint64_t inRegMask = generateEntryPointArgTys(shaderInputs, argTys, argNames, 0);

  Function *origEntryPoint = m_entryPoint;

  // Create the new function and transfer code and attributes to it.
  Function *entryPoint =
      addFunctionArgs(origEntryPoint, origEntryPoint->getFunctionType()->getReturnType(), argTys, argNames, inRegMask);

  // We always deal with pre-merge functions here, so set the fitting pre-merge calling conventions.
  switch (m_shaderStage) {
  case ShaderStageTask:
    entryPoint->setCallingConv(CallingConv::AMDGPU_CS);
    break;
  case ShaderStageMesh:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStageVertex:
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      entryPoint->setCallingConv(CallingConv::AMDGPU_LS);
    else if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStageTessControl:
    entryPoint->setCallingConv(CallingConv::AMDGPU_HS);
    break;
  case ShaderStageTessEval:
    if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStageGeometry:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStageFragment:
    entryPoint->setCallingConv(CallingConv::AMDGPU_PS);
    break;
  default:
    llvm_unreachable("unexpected shader stage for graphics shader");
  }

  // Set Attributes on new function.
  setFuncAttrs(entryPoint);

  // Remove original entry-point
  origEntryPoint->eraseFromParent();
}

// =====================================================================================================================
// Process all functions in a compute pipeline or library.
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs
// @param [in/out] module : Module
void PatchEntryPointMutate::processComputeFuncs(ShaderInputs *shaderInputs, Module &module) {
  m_shaderStage = ShaderStageCompute;

  // We no longer support compute shader fixed layout required before PAL interface version 624.
  if (m_pipelineState->getLgcContext()->getPalAbiVersion() < 624)
    report_fatal_error("Compute shader not supported before PAL version 624");

  // Process each function definition.
  SmallVector<Function *, 4> origFuncs;
  for (Function &func : module) {
    if (func.isDeclaration()) {
      if (!func.isIntrinsic() && !func.getName().startswith(lgcName::InternalCallPrefix)) {
        // This is the declaration of a callable function that is defined in a different module.
        func.setCallingConv(CallingConv::AMDGPU_Gfx);
      }
    } else {
      origFuncs.push_back(&func);
    }
  }

  for (Function *origFunc : origFuncs) {
    auto *origType = origFunc->getFunctionType();
    // Determine what args need to be added on to all functions.
    SmallVector<Type *, 20> shaderInputTys;
    SmallVector<std::string, 20> shaderInputNames;
    uint64_t inRegMask =
        generateEntryPointArgTys(shaderInputs, shaderInputTys, shaderInputNames, origType->getNumParams());

    // Create the new function and transfer code and attributes to it.
    Function *newFunc =
        addFunctionArgs(origFunc, origType->getReturnType(), shaderInputTys, shaderInputNames, inRegMask, true);
    const bool isEntryPoint = isShaderEntryPoint(newFunc);
    newFunc->setCallingConv(isEntryPoint ? CallingConv::AMDGPU_CS : CallingConv::AMDGPU_Gfx);

    // Set Attributes on new function.
    setFuncAttrs(newFunc);

    // Change any uses of the old function to a bitcast of the new function.
    SmallVector<Use *, 4> funcUses;
    for (auto &use : origFunc->uses())
      funcUses.push_back(&use);
    Constant *bitCastFunc = ConstantExpr::getBitCast(newFunc, origFunc->getType());
    for (Use *use : funcUses)
      *use = bitCastFunc;

    // Remove original function.
    int argOffset = origType->getNumParams();
    origFunc->eraseFromParent();

    if (isComputeWithCalls())
      processCalls(*newFunc, shaderInputTys, shaderInputNames, inRegMask, argOffset);
  }
}

// =====================================================================================================================
// Process all real function calls and passes arguments to them.
//
// @param [in/out] module : Module
void PatchEntryPointMutate::processCalls(Function &func, SmallVectorImpl<Type *> &shaderInputTys,
                                         SmallVectorImpl<std::string> &shaderInputNames, uint64_t inRegMask,
                                         unsigned argOffset) {
  // This is one of:
  // - a compute pipeline with non-inlined functions;
  // - a compute pipeline with calls to library functions;
  // - a compute library.
  // We need to scan the code and modify each call to append the extra args.
  IRBuilder<> builder(func.getContext());
  for (BasicBlock &block : func) {
    // Use early increment iterator, so we can safely erase the instruction.
    for (Instruction &inst : make_early_inc_range(block)) {
      auto call = dyn_cast<CallInst>(&inst);
      if (!call)
        continue;
      // Got a call. Skip it if it calls an intrinsic or an internal lgc.* function.
      Value *calledVal = call->getCalledOperand();
      Function *calledFunc = dyn_cast<Function>(calledVal);
      if (calledFunc) {
        if (calledFunc->isIntrinsic() || calledFunc->getName().startswith(lgcName::InternalCallPrefix))
          continue;
      } else if (call->isInlineAsm()) {
        continue;
      }
      // Build a new arg list, made of the ABI args shared by all functions (user data and hardware shader
      // inputs), plus the original args on the call.
      SmallVector<Type *, 20> argTys;
      SmallVector<Value *, 20> args;
      for (unsigned idx = 0; idx != call->arg_size(); ++idx) {
        argTys.push_back(call->getArgOperand(idx)->getType());
        args.push_back(call->getArgOperand(idx));
      }
      for (unsigned idx = 0; idx != shaderInputTys.size(); ++idx) {
        argTys.push_back(func.getArg(idx + argOffset)->getType());
        args.push_back(func.getArg(idx + argOffset));
      }
      // Get the new called value as a bitcast of the old called value. If the old called value is already
      // the inverse bitcast, just drop that bitcast.
      // If the old called value was a function declaration, we did not insert a bitcast
      FunctionType *calledTy = FunctionType::get(call->getType(), argTys, false);
      builder.SetInsertPoint(call);
      Type *calledPtrTy = calledTy->getPointerTo(calledVal->getType()->getPointerAddressSpace());
      auto bitCast = dyn_cast<BitCastOperator>(calledVal);
      Value *newCalledVal = nullptr;
      if (bitCast && bitCast->getOperand(0)->getType() == calledPtrTy)
        newCalledVal = bitCast->getOperand(0);
      else
        newCalledVal = builder.CreateBitCast(calledVal, calledPtrTy);
      // Create the call.
      CallInst *newCall = builder.CreateCall(calledTy, newCalledVal, args);
      newCall->setCallingConv(CallingConv::AMDGPU_Gfx);

      // Mark sgpr arguments as inreg
      for (unsigned idx = 0; idx != shaderInputTys.size(); ++idx) {
        if ((inRegMask >> idx) & 1)
          newCall->addParamAttr(idx + call->arg_size(), Attribute::InReg);
      }

      // Replace and erase the old one.
      call->replaceAllUsesWith(newCall);
      call->eraseFromParent();
    }
  }
}

// =====================================================================================================================
// Set Attributes on new function
void PatchEntryPointMutate::setFuncAttrs(Function *entryPoint) {
  AttrBuilder builder(entryPoint->getContext());
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

    bool hasDepthExport = builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth;
    builder.addAttribute("amdgpu-depth-export", hasDepthExport ? "1" : "0");

    bool hasColorExport = false;
    // SpiShaderColFormat / mmSPI_SHADER_COL_FORMAT is used for fully compiled shaders
    unsigned colFormat = EXP_FORMAT_ZERO;
    if (m_pipelineState->useRegisterFieldFormat()) {
      auto &colFormatNode = m_pipelineState->getPalMetadata()
                                ->getPipelineNode()
                                .getMap(true)[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                                .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderColFormat]
                                .getMap(true);
      for (auto iter = colFormatNode.begin(); iter != colFormatNode.end(); ++iter) {
        if (iter->second.getUInt() != EXP_FORMAT_ZERO) {
          colFormat = iter->second.getUInt();
          break;
        }
      }
    } else {
      colFormat = m_pipelineState->getPalMetadata()->getRegister(mmSPI_SHADER_COL_FORMAT);
    }
    if (colFormat != EXP_FORMAT_ZERO)
      hasColorExport = true;

    if (!hasColorExport) {
      // getColorExportCount() is used for partially compiled shaders
      const unsigned colorExportCount = m_pipelineState->getPalMetadata()->getColorExportCount();
      if (colorExportCount > static_cast<unsigned>(hasDepthExport))
        hasColorExport = true;
    }

    builder.addAttribute("amdgpu-color-export", hasColorExport ? "1" : "0");
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

  if (shaderOptions->ldsSpillLimitDwords != 0) {
    // Sanity check: LDS spilling is only supported in Fragment and Compute.
    if (m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageCompute)
      builder.addAttribute("amdgpu-lds-spill-limit-dwords", std::to_string(shaderOptions->ldsSpillLimitDwords));
  }

  if (shaderOptions->disableCodeSinking)
    builder.addAttribute("disable-code-sinking");

  if (shaderOptions->nsaThreshold != 0)
    builder.addAttribute("amdgpu-nsa-threshold", std::to_string(shaderOptions->nsaThreshold));

  // Disable backend heuristics which would allow shaders to have lower occupancy. Heed the favorLatencyHiding tuning
  // option instead.
  builder.addAttribute("amdgpu-memory-bound", shaderOptions->favorLatencyHiding ? "true" : "false");
  builder.addAttribute("amdgpu-wave-limiter", "false");

  entryPoint->addFnAttrs(builder);

  // NOTE: Remove "readnone" attribute for entry-point. If GS is empty, this attribute will allow
  // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 440919
  // Old version of the code
  if (entryPoint->hasFnAttribute(Attribute::ReadNone))
    entryPoint->removeFnAttr(Attribute::ReadNone);
#else
  // New version of the code (also handles unknown version, which we treat as
  // latest)
  entryPoint->setMemoryEffects(MemoryEffects::unknown());
#endif
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
// @param [out] argTys : The argument types for the new function type
// @param [out] argNames : The argument names corresponding to the argument types
// @returns inRegMask : "Inreg" bit mask for the arguments, with a bit set to indicate that the corresponding
//                          arg needs to have an "inreg" attribute to put the arg into SGPRs rather than VGPRs
//
uint64_t PatchEntryPointMutate::generateEntryPointArgTys(ShaderInputs *shaderInputs, SmallVectorImpl<Type *> &argTys,
                                                         SmallVectorImpl<std::string> &argNames, unsigned argOffset) {

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
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "globalTable", UserDataMapping::GlobalTable));

  // Per-shader table
  // TODO: We need add per shader table per real usage after switch to PAL new interface.
  // if (pResUsage->perShaderTable)
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "perShaderTable"));

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
      *userDataArg.argIndex = argTys.size() + argOffset;
    unsigned dwordSize = userDataArg.argDwordSize;
    if (userDataArg.userDataValue != static_cast<unsigned>(UserDataMapping::Invalid)) {
      // Most of user data metadata entries is 1 except for root push descriptors.
      bool isSystemUserData = isSystemUserDataValue(userDataArg.userDataValue);
      unsigned numEntries = isSystemUserData ? 1 : dwordSize;
      assert((!isUnlinkedDescriptorSetValue(userDataArg.userDataValue) || dwordSize == 1) &&
             "Expecting descriptor set values to be one dword.  The linker cannot handle anything else.");
      if (!m_pipelineState->useRegisterFieldFormat())
        m_pipelineState->getPalMetadata()->setUserDataEntry(m_shaderStage, userDataIdx, userDataArg.userDataValue,
                                                            numEntries);
      if (isSystemUserData) {
        unsigned index = userDataArg.userDataValue - static_cast<unsigned>(UserDataMapping::GlobalTable);
        auto &specialUserData = getUserDataUsage(m_shaderStage)->specialUserData;
        if (index < specialUserData.size())
          specialUserData[index].entryArgIdx = argTys.size() + argOffset;
      }
    }
    argTys.push_back(userDataArg.argTy);
    argNames.push_back(userDataArg.name);
    userDataIdx += dwordSize;
  }

  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waUserSgprInitBug) {
    // Add dummy user data to bring the total to 16 SGPRS if hardware workaround
    // is required

    // Only applies to wave32
    // TODO: Can we further exclude PS if LDS_GROUP_SIZE == 0
    if (m_pipelineState->getShaderWaveSize(m_shaderStage) == 32 &&
        (m_shaderStage == ShaderStageCompute || m_shaderStage == ShaderStageFragment ||
         m_shaderStage == ShaderStageMesh)) {
      unsigned userDataLimit = m_shaderStage == ShaderStageMesh ? 8 : 16;

      while (userDataIdx < userDataLimit) {
        argTys.push_back(builder.getInt32Ty());
        argNames.push_back(("dummyInit" + Twine(userDataIdx)).str());
        userDataIdx += 1;
      }
    }
  }

  intfData->userDataCount = userDataIdx;
  inRegMask = (1ull << argTys.size()) - 1;

  // Push the fixed system (not user data) register args.
  inRegMask |= shaderInputs->getShaderArgTys(m_pipelineState, m_shaderStage, argTys, argNames, argOffset);

  if (m_pipelineState->useRegisterFieldFormat()) {
    constexpr unsigned NumUserSgprs = 32;
    constexpr unsigned InvalidMapVal = static_cast<unsigned>(UserDataMapping::Invalid);
    SmallVector<unsigned, NumUserSgprs> userDataMap;
    userDataMap.resize(NumUserSgprs, InvalidMapVal);
    userDataIdx = 0;
    for (const auto &userDataArg : unspilledArgs) {
      unsigned dwordSize = userDataArg.argDwordSize;
      if (userDataArg.userDataValue != InvalidMapVal) {
        bool isSystemUserData = isSystemUserDataValue(userDataArg.userDataValue);
        unsigned numEntries = isSystemUserData ? 1 : dwordSize;
        unsigned userDataValue = userDataArg.userDataValue;
        unsigned idx = userDataIdx;
        while (numEntries--)
          userDataMap[idx++] = userDataValue++;
      }
      userDataIdx += dwordSize;
    }
    m_pipelineState->setUserDataMap(m_shaderStage, userDataMap);
  }

  return inRegMask;
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to a special system user data value.
bool PatchEntryPointMutate::isSystemUserDataValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::GlobalTable)) {
    return false;
  }
  return userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0);
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to an unlinked descriptor set.
bool PatchEntryPointMutate::isUnlinkedDescriptorSetValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0)) {
    return false;
  }
  return userDataValue <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax);
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
  auto &entryArgIdxs = intfData->entryArgIdxs;
  bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessControl ||
      m_shaderStage == ShaderStageTessEval || m_shaderStage == ShaderStageGeometry) {
    // Shader stage in the vertex-processing half of a graphics pipeline.
    // We need to ensure that the layout is the same between two shader stages that will be merged on GFX9+,
    // that is, VS-TCS, VS-GS (if no tessellation), TES-GS.

    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (m_pipelineState->getInputAssemblyState().enableMultiView) {
      unsigned *argIdx = nullptr;
      auto userDataValue = UserDataMapping::ViewId;
      switch (m_shaderStage) {
      case ShaderStageVertex:
        argIdx = &entryArgIdxs.vs.viewIndex;
        break;
      case ShaderStageTessControl:
        argIdx = &entryArgIdxs.tcs.viewIndex;
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
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "viewId", userDataValue, argIdx));
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
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "esGsLdsSize", userDataValue));
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
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "vertexBufferTable",
                                                  UserDataMapping::VertexBufferTable,
                                                  &vsIntfData->entryArgIdxs.vs.vbTablePtr));
      }

      // Base vertex and base instance.
      if (willHaveFetchShader || vsResUsage->builtInUsage.vs.baseVertex || vsResUsage->builtInUsage.vs.baseInstance ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseVertex) ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseInstance)) {
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "baseVertex", UserDataMapping::BaseVertex,
                                                  &vsIntfData->entryArgIdxs.vs.baseVertex));
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "baseInstance", UserDataMapping::BaseInstance,
                                                  &vsIntfData->entryArgIdxs.vs.baseInstance));
      }

      // Draw index.
      if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::DrawIndex))
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex));
    }

  } else if (m_shaderStage == ShaderStageCompute) {
    // Pass the gl_NumWorkgroups pointer in user data registers.
    // Always enable this, even if unused, if compute library is in use.
    // Unlike all the special user data values above, which go after the user data node args, this goes before.
    // That is to ensure that, with a compute pipeline using a library, library code knows where to find it
    // even if it thinks that the user data layout is a prefix of what the pipeline thinks it is.
    if (isComputeWithCalls() || userDataUsage->isSpecialUserDataUsed(UserDataMapping::Workgroup)) {
      auto numWorkgroupsPtrTy = PointerType::get(FixedVectorType::get(builder.getInt32Ty(), 3), ADDR_SPACE_CONST);
      userDataArgs.push_back(UserDataArg(numWorkgroupsPtrTy, "numWorkgroupsPtr", UserDataMapping::Workgroup, nullptr));
    }
  } else if (m_shaderStage == ShaderStageTask) {
    // Draw index.
    if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::DrawIndex))
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex));

    specialUserDataArgs.push_back(UserDataArg(FixedVectorType::get(builder.getInt32Ty(), 3), "meshTaskDispatchDims",
                                              UserDataMapping::MeshTaskDispatchDims,
                                              &intfData->entryArgIdxs.task.dispatchDims));
    specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshTaskRingIndex",
                                              UserDataMapping::MeshTaskRingIndex,
                                              &intfData->entryArgIdxs.task.baseRingEntryIndex));
    if (m_pipelineState->needSwMeshPipelineStats()) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshPipeStatsBuf",
                                                UserDataMapping::MeshPipeStatsBuf,
                                                &intfData->entryArgIdxs.task.pipeStatsBuf));
    }
  } else if (m_shaderStage == ShaderStageMesh) {
    if (m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh.drawIndex) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex,
                                                &intfData->entryArgIdxs.mesh.drawIndex));
    }
    if (m_pipelineState->getInputAssemblyState().enableMultiView) {
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), "viewId", UserDataMapping::ViewId, &intfData->entryArgIdxs.mesh.viewIndex));
    }
    specialUserDataArgs.push_back(UserDataArg(FixedVectorType::get(builder.getInt32Ty(), 3), "meshTaskDispatchDims",
                                              UserDataMapping::MeshTaskDispatchDims,
                                              &intfData->entryArgIdxs.mesh.dispatchDims));
    specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshTaskRingIndex",
                                              UserDataMapping::MeshTaskRingIndex,
                                              &intfData->entryArgIdxs.mesh.baseRingEntryIndex));
    if (m_pipelineState->needSwMeshPipelineStats()) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshPipeStatsBuf",
                                                UserDataMapping::MeshPipeStatsBuf,
                                                &intfData->entryArgIdxs.mesh.pipeStatsBuf));
    }
  } else if (m_shaderStage == ShaderStageFragment) {
    if (m_pipelineState->getInputAssemblyState().enableMultiView &&
        m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs.viewIndex) {
      // NOTE: Only add special user data of view index when multi-view is enabled and gl_ViewIndex is used in fragment
      // shader.
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), "viewId", UserDataMapping::ViewId, &intfData->entryArgIdxs.fs.viewIndex));
    }
  }

  // Allocate register for stream-out buffer table, to go before the user data node args (unlike all the ones
  // above, which go after the user data node args).
  if (userDataUsage->usesStreamOutTable || userDataUsage->isSpecialUserDataUsed(UserDataMapping::StreamOutTable)) {
    if (enableNgg || !m_pipelineState->hasShaderStage(ShaderStageCopyShader) && m_pipelineState->enableXfb()) {
      // If no NGG, stream out table will be set to copy shader's user data entry, we should not set it duplicately.
      switch (m_shaderStage) {
      case ShaderStageVertex:
        userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutTable", UserDataMapping::StreamOutTable,
                                           &intfData->entryArgIdxs.vs.streamOutData.tablePtr));
        if (m_pipelineState->enableSwXfb()) {
          // NOTE: For GFX11+, the SW stream-out needs an additional special user data SGPR to store the
          // stream-out control buffer address.
          specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutControlBuf",
                                                    UserDataMapping::StreamOutControlBuf,
                                                    &intfData->entryArgIdxs.vs.streamOutData.controlBufPtr));
        }
        break;
      case ShaderStageTessEval:
        userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutTable", UserDataMapping::StreamOutTable,
                                           &intfData->entryArgIdxs.tes.streamOutData.tablePtr));
        if (m_pipelineState->enableSwXfb()) {
          // NOTE: For GFX11+, the SW stream-out needs an additional special user data SGPR to store the
          // stream-out control buffer address.
          specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutControlBuf",
                                                    UserDataMapping::StreamOutControlBuf,
                                                    &intfData->entryArgIdxs.tes.streamOutData.controlBufPtr));
        }
        break;
      case ShaderStageGeometry:
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 10) {
          // Allocate dummy stream-out register for geometry shader
          userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "dummyStreamOut"));
        } else if (m_pipelineState->enableSwXfb()) {
          userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutTable", UserDataMapping::StreamOutTable,
                                             &intfData->entryArgIdxs.gs.streamOutData.tablePtr));
          // NOTE: For GFX11+, the SW stream-out needs an additional special user data SGPR to store the
          // stream-out control buffer address.
          specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutControlBuf",
                                                    UserDataMapping::StreamOutControlBuf,
                                                    &intfData->entryArgIdxs.gs.streamOutData.controlBufPtr));
        }
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
    for (unsigned descSetIdx = 0; descSetIdx != userDataUsage->descriptorTables.size(); ++descSetIdx) {
      auto &descriptorTable = userDataUsage->descriptorTables[descSetIdx];
      if (!descriptorTable.users.empty()) {
        // Set the PAL metadata user data value to indicate that it needs modifying at link time.
        assert(descSetIdx <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax) -
                                 static_cast<unsigned>(UserDataMapping::DescriptorSet0));
        unsigned userDataValue = static_cast<unsigned>(UserDataMapping::DescriptorSet0) + descSetIdx;
        userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "descTable" + Twine(descSetIdx), userDataValue,
                                           &descriptorTable.entryArgIdx));
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
                     pushConstOffset.dwordSize, "pushConst" + Twine(dwordOffset), &pushConstOffset.entryArgIdx,
                     builder);
    }

    return;
  }

  // We do have user data layout.
  // Add entries from the root user data layout (not vertex buffer or streamout, and not unused ones).

  llvm::ArrayRef<ResourceNode> userDataNodes = m_pipelineState->getUserDataNodes();
  for (unsigned userDataNodeIdx = 0; userDataNodeIdx != userDataNodes.size(); ++userDataNodeIdx) {
    const ResourceNode &node = userDataNodes[userDataNodeIdx];
    switch (node.concreteType) {

    case ResourceNodeType::IndirectUserDataVaPtr:
    case ResourceNodeType::StreamOutTableVaPtr:
      break;

    case ResourceNodeType::DescriptorTableVaPtr: {
      // Check if the descriptor set is in use. For compute with calls, enable it anyway.
      UserDataNodeUsage *descSetUsage = nullptr;
      if (userDataUsage->descriptorTables.size() > userDataNodeIdx)
        descSetUsage = &userDataUsage->descriptorTables[userDataNodeIdx];
      if (!isComputeWithCalls() && (!descSetUsage || descSetUsage->users.empty()))
        break;

      unsigned userDataValue = node.offsetInDwords;
      if (m_pipelineState->getShaderOptions(m_shaderStage).updateDescInElf && m_shaderStage == ShaderStageFragment) {
        // Put set number to register first, will update offset after merge ELFs
        // For partial pipeline compile, only fragment shader needs to adjust offset of root descriptor.
        // This is part of the original "partial pipeline compile" scheme, and it uses a magic number for the
        // PAL metadata register value because the code to fix it up in llpcElfWriter.cpp just fixes up any
        // register with the magic value, and hopes it lucks out by not getting a false positive.
        // TODO: Remove all that code once the new "shader/part-pipeline compile" scheme can replace it.
        static const unsigned DescRelocMagic = 0xA5A5A500;
        userDataValue = DescRelocMagic | node.innerTable[0].set;
      }
      // Add the arg (descriptor set pointer) that we can potentially unspill.
      unsigned *argIndex = descSetUsage == nullptr ? nullptr : &descSetUsage->entryArgIdx;
      addUserDataArg(userDataArgs, userDataValue, node.sizeInDwords, "descTable" + Twine(userDataNodeIdx), argIndex,
                     builder);
      break;
    }

    case ResourceNodeType::PushConst: {
      // Always spill for compute libraries.
      if (!isComputeWithCalls()) {
        // We add a potential unspilled arg for each separate dword offset of the push const at which there is a load.
        // We already know that loads we have on our pushConstOffsets lists are at dword-aligned offset and
        // dword-aligned size. We need to ensure that all loads are the same size, by removing ones that are bigger than
        // the minimum size.
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
                         "pushConst" + Twine(dwordOffset), &pushConstOffset.entryArgIdx, builder);
        }
      } else {
        // Mark push constant for spill for compute library.
        userDataUsage->pushConstSpill = true;
      }

      // Ensure we mark the push constant's part of the spill table as used.
      if (userDataUsage->pushConstSpill)
        userDataUsage->spillUsage = std::min(userDataUsage->spillUsage, node.offsetInDwords);

      break;
    }

    default:
      if (isComputeWithCalls()) {
        // Always spill for compute libraries.
        break;
      }

      for (unsigned dwordOffset = node.offsetInDwords; dwordOffset != node.offsetInDwords + node.sizeInDwords;
           ++dwordOffset) {
        if (userDataUsage->rootDescriptors.size() <= dwordOffset)
          break;
        auto &rootDescUsage = userDataUsage->rootDescriptors[dwordOffset];
        // Skip unused descriptor.
        if (rootDescUsage.users.empty())
          continue;
        unsigned dwordSize = rootDescUsage.users[0]->getType()->getPrimitiveSizeInBits() / 32;
        // Add the arg (root descriptor) that we can potentially unspill.
        addUserDataArg(userDataArgs, dwordOffset, dwordSize, "rootDesc" + Twine(dwordOffset),
                       &rootDescUsage.entryArgIdx, builder);
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
                                           unsigned sizeInDwords, const Twine &name, unsigned *argIndex,
                                           IRBuilder<> &builder) {
  Type *argTy = builder.getInt32Ty();
  if (sizeInDwords != 1)
    argTy = FixedVectorType::get(argTy, sizeInDwords);
  userDataArgs.push_back(UserDataArg(argTy, name, userDataValue, argIndex));
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
// @param [out] unspilledArgNames : Argument names of unspilled arguments.
void PatchEntryPointMutate::determineUnspilledUserDataArgs(ArrayRef<UserDataArg> userDataArgs,
                                                           ArrayRef<UserDataArg> specialUserDataArgs,
                                                           IRBuilder<> &builder,
                                                           SmallVectorImpl<UserDataArg> &unspilledArgs) {

  std::optional<UserDataArg> spillTableArg;

  auto userDataUsage = getUserDataUsage(m_shaderStage);
  if (!userDataUsage->spillTable.users.empty() || userDataUsage->pushConstSpill ||
      userDataUsage->spillUsage != UINT_MAX) {
    // Spill table is already in use by code added in DescBuilder, or by uses of the push const pointer not
    // all being of the form that can be unspilled.
    spillTableArg = UserDataArg(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                                &userDataUsage->spillTable.entryArgIdx);

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

  // In compute-with-calls, we need to ensure that the compute shader and library code agree that s15 is the spill
  // table pointer, even if it is not needed, because library code does not know whether a spill table pointer is
  // needed in the pipeline. Thus we cannot use s15 for anything else. Using the single-arg UserDataArg
  // constructor like this means that the arg is not used, so it will not be set up in PAL metadata.
  if (m_computeWithCalls && !spillTableArg.has_value())
    spillTableArg = UserDataArg(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                                &userDataUsage->spillTable.entryArgIdx);

  // Figure out how many sgprs we have available for userDataArgs.
  // We have s0-s31 (s0-s15 for <=GFX8, or for a compute/task shader on any chip) for everything, so take off the number
  // of registers used by specialUserDataArgs.
  unsigned userDataEnd = (m_shaderStage == ShaderStageCompute || m_shaderStage == ShaderStageTask)
                             ? InterfaceData::MaxCsUserDataCount
                             : m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;

  // FIXME Restricting user data as the backend does not support more sgprs as arguments
  if (isComputeWithCalls() && userDataEnd > 16)
    userDataEnd = 16;

  for (auto &userDataArg : specialUserDataArgs)
    userDataEnd -= userDataArg.argDwordSize;
  // ... and the one used by the spill table if already added.
  if (spillTableArg.has_value())
    userDataEnd -= 1;

  // See if we need to spill any user data nodes in userDataArgs, copying the unspilled ones across to unspilledArgs.
  unsigned userDataIdx = 0;

  for (const UserDataArg &userDataArg : userDataArgs) {
    unsigned afterUserDataIdx = userDataIdx + userDataArg.argDwordSize;
    if (afterUserDataIdx > userDataEnd) {
      // Spill this node. Allocate the spill table arg.
      if (!spillTableArg.has_value()) {
        spillTableArg = UserDataArg(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                                    &userDataUsage->spillTable.entryArgIdx);
        --userDataEnd;

        if (userDataIdx > userDataEnd) {
          // We over-ran the available SGPRs by filling them up and then realizing we needed a spill table pointer.
          // Remove the last unspilled node (and any padding arg before that), and ensure that spill usage is
          // set correctly so that PAL metadata spill threshold is correct.
          // (Note that this path cannot happen in compute-with-calls, because we pre-reserved a slot for the
          // spill table pointer.)
          userDataIdx -= unspilledArgs.back().argDwordSize;
          userDataUsage->spillUsage = std::min(userDataUsage->spillUsage, unspilledArgs.back().userDataValue);
          unspilledArgs.pop_back();
        }
      } else if (!spillTableArg->argIndex) {
        // This is the compute-with-calls case that we reserved s15 for the spill table pointer above,
        // without setting its PAL metadata or spillTable.entryArgIdx, but now we find we do need to set
        // them.
        spillTableArg = UserDataArg(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                                    &userDataUsage->spillTable.entryArgIdx);
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

  // For compute-with-calls, add extra padding unspilled args until we get to s15. s15 will then be used for
  // the spill table pointer below, even if we didn't appear to need one.
  if (isComputeWithCalls()) {
    while (userDataIdx < userDataEnd) {
      unspilledArgs.push_back(UserDataArg(builder.getInt32Ty(), Twine()));
      ++userDataIdx;
    }
  }

  // Add the special args and the spill table pointer (if any) to unspilledArgs.
  // (specialUserDataArgs is empty for compute, and thus for compute-with-calls.)
  unspilledArgs.insert(unspilledArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
  if (spillTableArg.has_value())
    unspilledArgs.insert(unspilledArgs.end(), *spillTableArg);
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
bool PatchEntryPointMutate::isComputeWithCalls() const {
  return m_computeWithCalls;
}

// =====================================================================================================================
bool PatchEntryPointMutate::UserDataUsage::isSpecialUserDataUsed(UserDataMapping kind) {
  unsigned index = static_cast<unsigned>(kind) - static_cast<unsigned>(UserDataMapping::GlobalTable);
  return specialUserData.size() > index && !specialUserData[index].users.empty();
}
