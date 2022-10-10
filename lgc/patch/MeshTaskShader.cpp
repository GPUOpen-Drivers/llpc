/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MeshTaskShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::MeshTaskShader.
 ***********************************************************************************************************************
 */
#include "MeshTaskShader.h"
#include "Gfx9Chip.h"
#include "ShaderMerger.h"
#include "lgc/patch/Patch.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "lgc-mesh-task-shader"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Constructor
//
// @param pipelineState : Pipeline state
MeshTaskShader::MeshTaskShader(PipelineState *pipelineState)
    : m_pipelineState(pipelineState), m_builder(std::make_unique<IRBuilder<>>(pipelineState->getContext())),
      m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()) {
  assert(pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  m_pipelineSysValues.initialize(m_pipelineState);
}

// =====================================================================================================================
// Destructor
MeshTaskShader::~MeshTaskShader() {
  m_pipelineSysValues.clear();
}

// =====================================================================================================================
// Layout mesh shader LDS if 'ldsLayout' is specified and calculate the required total LDS size (in dwords).
//
// @param pipelineState : Pipeline state
// @param entryPoint : Entry-point of mesh shader
// @param ldsLayout : Mesh shader LDS layout (could be null)
unsigned MeshTaskShader::layoutMeshShaderLds(PipelineState *pipelineState, Function *entryPoint,
                                             MeshLdsLayout *ldsLayout) {
  if (!pipelineState->hasShaderStage(ShaderStageMesh))
    return 0; // Mesh shader absent (standalone compiler tries to compile a single task shader)

  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  (void(gfxIp));                          // Unused

  //
  // The LDS layout of mesh shader is something as follow (consists of two main parts):
  //
  // 1. Internal mesh LDS:
  //
  // +--------------+-----------------+-------------------+-------------------+----------------+-------------------+
  // | Vertex Count | Primitive Count | Flat Workgroup ID | Primitive Indices | Vertex Outputs | Primitive Outputs |
  // +--------------+-----------------+-------------------+-------------------+----------------+-------------------+
  //
  // 2. Shared variable LDS:
  //
  // +------------------+
  // | Shared Variables |
  // +------------------+
  //

  auto meshMode = pipelineState->getShaderModes()->getMeshShaderMode();
  assert(meshMode.outputVertices <= Gfx9::NggMaxThreadsPerSubgroup);
  assert(meshMode.outputPrimitives <= Gfx9::NggMaxThreadsPerSubgroup);

  const auto resUsage = pipelineState->getShaderResourceUsage(ShaderStageMesh);

  unsigned meshLdsSizeInDwords = 0;
  unsigned ldsOffsetInDwords = 0;
  unsigned ldsRegionSize = 0;

  auto printLdsRegionInfo = [=](const char *regionName, unsigned regionOffset, unsigned regionSize) {
    LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, regionName, regionOffset, regionSize));
    if (regionSize == 0)
      LLPC_OUTS(" (empty)");
    LLPC_OUTS("\n");
  };

  if (ldsLayout) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC mesh shader LDS region info (in dwords) and general info\n\n");
  }

  // Vertex count
  ldsRegionSize = 1; //  A dword corresponds to vertex count (i32)
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Count", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::VertexCount] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Primitive count
  ldsRegionSize = 1; // A dword corresponds to primitive count (i32)
  if (ldsLayout) {
    printLdsRegionInfo("Primitive Count", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::PrimitiveCount] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Flat workgroup ID
  if (useFlatWorkgroupId(pipelineState)) {
    ldsRegionSize = 1; // A dword corresponds to flat workgroup ID (i32)
    if (ldsLayout) {
      printLdsRegionInfo("Flat workgroup ID", ldsOffsetInDwords, ldsRegionSize);
      (*ldsLayout)[MeshLdsRegion::FlatWorkgroupId] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
      ldsOffsetInDwords += ldsRegionSize;
    }
    meshLdsSizeInDwords += ldsRegionSize;
  }

  // Primitive indices
  ldsRegionSize = meshMode.outputPrimitives; // Each dword corresponds to primitive connectivity data (i32)
  if (ldsLayout) {
    printLdsRegionInfo("Primitive Indices", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::PrimitiveIndices] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Per-vertex outputs
  const unsigned vertexStride = 4 * resUsage->inOutUsage.outputMapLocCount; // Corresponds to vec4 output
  ldsRegionSize = vertexStride * meshMode.outputVertices;
  if (ldsLayout) {
    printLdsRegionInfo("Per-vertex Output", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::VertexOutput] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Per-primitive outputs
  const unsigned primitiveStride = 4 * resUsage->inOutUsage.perPrimitiveOutputMapLocCount; // Corresponds to vec4 output
  ldsRegionSize = primitiveStride * meshMode.outputPrimitives;
  if (ldsLayout) {
    printLdsRegionInfo("Per-primitive Output", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::PrimitiveOutput] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Check shared variables
  SmallVector<GlobalVariable *, 8> meshSharedVars;
  for (auto &global : entryPoint->getParent()->globals()) {
    if (global.getType()->getAddressSpace() != ADDR_SPACE_LOCAL)
      continue; // Not shared variable (Shared variables are always mapped to LDS)

    for (auto user : global.users()) {
      bool found = false;
      if (auto inst = dyn_cast<Instruction>(user)) {
        if (inst->getFunction() == entryPoint)
          found = true;
      } else {
        assert(isa<ConstantExpr>(user)); // Must be constant expression
        for (auto userOfUser : user->users()) {
          auto inst = cast<Instruction>(userOfUser); // Must be instruction
          if (inst->getFunction() == entryPoint) {
            found = true;
            break;
          }
        }
      }

      if (found) {
        meshSharedVars.push_back(&global);
        break;
      }
    }
  }

  unsigned sharedVarLdsSizeInDwords = 0;
  for (auto meshSharedVar : meshSharedVars) {
    assert(meshSharedVar->getAlignment() == 4); // Must be 1 dword
    const auto sizeInBytes =
        meshSharedVar->getParent()->getDataLayout().getTypeAllocSize(meshSharedVar->getValueType());
    assert(sizeInBytes % 4 == 0); // Must be multiple of 4
    sharedVarLdsSizeInDwords += sizeInBytes / 4;
  }

  // Setup internal mesh LDS
  getOrCreateMeshLds(entryPoint->getParent(), meshLdsSizeInDwords);

  if (ldsLayout) {
    LLPC_OUTS("\n");
    printLdsRegionInfo("Internal Mesh LDS", 0, meshLdsSizeInDwords);
    printLdsRegionInfo("Shared Variable LDS", 0, sharedVarLdsSizeInDwords);
    printLdsRegionInfo("Total LDS", 0, meshLdsSizeInDwords + sharedVarLdsSizeInDwords);
    LLPC_OUTS("\n");
    LLPC_OUTS("Workgroup Size (X, Y, Z) = (" << meshMode.workgroupSizeX << ", " << meshMode.workgroupSizeY << ", "
                                             << meshMode.workgroupSizeZ << ")\n");
    LLPC_OUTS("NumMeshThreads = " << meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ
                                  << "\n");
    LLPC_OUTS("Primitives = ");
    switch (meshMode.outputPrimitive) {
    case OutputPrimitives::Points:
      LLPC_OUTS("Points\n");
      break;
    case OutputPrimitives::Lines:
      LLPC_OUTS("Points\n");
      break;
    case OutputPrimitives::Triangles:
      LLPC_OUTS("Triangles\n");
      break;
    default:
      llvm_unreachable("Unknown primitive type");
      LLPC_OUTS("Unknown\n");
      break;
    }
    LLPC_OUTS("Max Vertices = " << meshMode.outputVertices << ", Max Primitives = " << meshMode.outputPrimitives
                                << "\n");
    if (!meshSharedVars.empty()) {
      LLPC_OUTS("Shared variables:\n");
      for (auto meshSharedVar : meshSharedVars) {
        assert(meshSharedVar->getAlignment() == 4); // Must be 1 dword
        const auto sizeInBytes =
            meshSharedVar->getParent()->getDataLayout().getTypeAllocSize(meshSharedVar->getValueType());
        assert(sizeInBytes % 4 == 0); // Must be multiple of 4
        const auto sizeInDwords = sizeInBytes / 4;

        LLPC_OUTS("Name = " << meshSharedVar->getName() << ", Type = " << getTypeName(meshSharedVar->getValueType())
                            << ", Size (in dwords) = " << sizeInDwords << "\n");
      }
    }
    LLPC_OUTS("\n");
  }

  return meshLdsSizeInDwords + sharedVarLdsSizeInDwords;
}

// =====================================================================================================================
// Process the mesh/task shader lowering.
//
// @param taskEntryPoint : Entry-point of task shader (could be null)
// @param meshEntryPoint : Entry-point of mesh shader (could be null)
void MeshTaskShader::process(Function *taskEntryPoint, Function *meshEntryPoint) {
  if (taskEntryPoint)
    processTaskShader(taskEntryPoint);

  if (meshEntryPoint)
    processMeshShader(meshEntryPoint);
}

// =====================================================================================================================
// Get or create global variable for internal mesh LDS.
//
// @param module : Module to get or create internal mesh LDS
// @param meshLdsSizeInDwords : Size of internal mesh LDS in dwords (optional)
GlobalVariable *MeshTaskShader::getOrCreateMeshLds(Module *module, unsigned meshLdsSizeInDwords) {
  static const char *MeshLdsName = "MeshLds"; // Name of internal mesh LDS

  // See if this module already has this LDS
  auto meshLds = module->getNamedValue(MeshLdsName);
  if (meshLds)
    return cast<GlobalVariable>(meshLds);

  // Now we can create the LDS
  assert(meshLdsSizeInDwords > 0);
  auto meshLdsTy = ArrayType::get(Type::getInt32Ty(module->getContext()), meshLdsSizeInDwords);
  auto newMeshLds = new GlobalVariable(*module, meshLdsTy, false, GlobalValue::ExternalLinkage, nullptr, MeshLdsName,
                                       nullptr, GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
  newMeshLds->setAlignment(MaybeAlign(sizeof(unsigned)));
  return newMeshLds;
}

// =====================================================================================================================
// Check whether flat workgroup ID will be used directly or indirectly in mesh shader.
//
// @param pipelineState : Pipeline state
// @returns : The flag indicating whether flat workgroup ID is used.
unsigned MeshTaskShader::useFlatWorkgroupId(PipelineState *pipelineState) {
  const auto &builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  return builtInUsage.workgroupId || builtInUsage.globalInvocationId;
}

// =====================================================================================================================
// Process task shader lowering.
//
// @param entryPoint : Entry-point of task shader
void MeshTaskShader::processTaskShader(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  //
  // NOTE: The processing is something like this:
  //
  // Task_Shader() {
  //   Initialize thread/wave info
  //
  //   Task shader main body (from API shader, lower task payload read/write)
  //
  //   Barrier
  //   if (threadIdInSubgroup == 0) {
  //     Write data to mesh pipeline statistics buffer
  //
  //     Lower EmitMeshTasks, read data from/write data to task draw data ring buffer, perform atomic operations on
  //     data in task draw data ring buffer
  //   }
  // }
  //
  m_builder->SetInsertPointPastAllocas(entryPoint);
  initWaveThreadInfo(entryPoint);

  SmallVector<CallInst *, 8> removedCalls;

  auto module = entryPoint->getParent();
  for (auto &func : module->functions()) {
    if (!func.isDeclaration())
      continue; // Not targets

    if (func.getName().startswith(lgcName::MeshTaskCallPrefix)) {
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);

        if (call->getFunction() != entryPoint)
          continue; // Not belong to task shader

        m_builder->SetInsertPoint(call);

        if (func.getName().startswith(lgcName::MeshTaskReadTaskPayload)) {
          // Read task payload
          assert(call->arg_size() == 1);
          auto byteOffset = call->getOperand(0);

          auto readValue = readTaskPayload(call->getType(), byteOffset);
          call->replaceAllUsesWith(readValue);
          m_accessTaskPayload = true;
        } else if (func.getName().startswith(lgcName::MeshTaskWriteTaskPayload)) {
          // Write task payload
          assert(call->arg_size() == 2);
          auto byteOffset = call->getOperand(0);
          auto writeValue = call->getOperand(1);

          writeTaskPayload(writeValue, byteOffset);
          m_accessTaskPayload = true;
        } else if (func.getName().startswith(lgcName::MeshTaskEmitMeshTasks)) {
          // Emit mesh tasks
          assert(call->arg_size() == 3);
          auto groupCountX = call->getOperand(0);
          auto groupCountY = call->getOperand(1);
          auto groupCountZ = call->getOperand(2);

          emitTaskMeshs(groupCountX, groupCountY, groupCountZ);
        } else if (func.getName().startswith(lgcName::MeshTaskAtomicTaskPayload)) {
          // Task payload atomic
          assert(call->arg_size() == 4);
          unsigned atomicOp = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
          AtomicOrdering ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(call->getOperand(1))->getZExtValue());
          auto inputValue = call->getOperand(2);
          auto byteOffset = call->getOperand(3);

          Value *atomicCall = taskPayloadAtomic(atomicOp, ordering, inputValue, byteOffset);
          call->replaceAllUsesWith(atomicCall);
          m_accessTaskPayload = true;
        } else if (func.getName().startswith(lgcName::MeshTaskAtomicCompareSwapTaskPayload)) {
          // Task payload atomic compare swap
          assert(call->arg_size() == 4);
          AtomicOrdering ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
          auto inputValue = call->getOperand(1);
          auto comparatorValue = call->getOperand(2);
          auto byteOffset = call->getOperand(3);

          Value *atomicCall = taskPayloadAtomicCompareSwap(ordering, inputValue, comparatorValue, byteOffset);
          call->replaceAllUsesWith(atomicCall);
          m_accessTaskPayload = true;
        } else {
          llvm_unreachable("Unknown task shader call!");
        }

        removedCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Process mesh shader lowering.
//
// @param entryPoint : Entry-point of mesh shader
void MeshTaskShader::processMeshShader(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  //
  // NOTE: The processing is something like this:
  //
  // Mesh_Shader() {
  //   Initialize thread/wave info
  //
  //   if (primitiveIndex < maxPrimitives)
  //     Zero primitive connectivity data
  //
  //   if (threadIdInSubgroup == 0) {
  //     Write invalid vertex count (~0) to LDS
  //     Write flat workgroup ID to LDS
  //   }
  //
  //   Barrier
  //   if (threadIdInSubgroup < numMeshThreads) {
  //     Mesh shader main body (from API shader, lower mesh shader specific calls)
  //       - SetMeshOutputs -> Write vertex/primitive count to LDS and send message GS_ALLOC_REQ
  //         (threadIdInSubgroup == 0)
  //       - SetPrimitiveIndices -> Write primitive connectivity data to LDS
  //       - SetPrimitiveCulled -> Write null primitive flag to LDS
  //       - GetMeshInput -> Lower mesh built-in input
  //       - ReadTaskPayload -> Read task payload from payload ring
  //       - Write primitive/vertex output -> Write output data to LDS
  //   }
  //
  //   Barrier
  //   Read vertex/primitive count from LDS
  //
  //   if (vertexCount == ~0) {
  //     if (threadIdInSubgroup == 0)
  //       Send message GS_ALLOC_REQ (vertexCount = 0, primitiveCount = 0)
  //     return
  //   }
  //
  //   if (primitiveIndex < primitiveCount) {
  //     Read primitive connectivity data from LDS
  //     Read primitive built-ins from LDS
  //     Export primitive
  //
  //     Read primitive attributes from LDS
  //     Export primitive attributes
  //   }
  //
  //   if (vertexIndex < vertexCount) {
  //     Read vertex built-ins from LDS
  //     Export vertex position data
  //
  //     Read vertex attributes from LDS
  //     Export vertex attributes
  //   }
  //
  //   if (threadIdInSubgroup == 0)
  //     Write data to mesh pipeline statistics buffer
  //
  //   return
  // }
  //

  // NOTE: We have to reset these two members since they might have stale values left by task shader processing.
  m_shaderRingEntryIndex = nullptr;
  m_payloadRingEntryOffset = nullptr;

  auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageMesh);

  // Setup LDS layout
  layoutMeshShaderLds(m_pipelineState, entryPoint, &m_ldsLayout);
  m_lds = getOrCreateMeshLds(entryPoint->getParent());

  // Mutate mesh shader entry-point
  entryPoint = mutateMeshShaderEntryPoint(entryPoint);

  // Force s_barrier to be present if necessary (ignore optimization)
  const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  auto primAmpFactor =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.primAmpFactor;
  // If we enable row export, the actual thread group size is determined by work group size provided from API mesh
  // shader.
  const unsigned flatWorkgroupSize = m_pipelineState->enableMeshRowExport() ? numMeshThreads : primAmpFactor;
  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        std::to_string(primAmpFactor) + std::string(",") + std::to_string(flatWorkgroupSize));

  const unsigned numWaves = alignTo(flatWorkgroupSize, waveSize) / waveSize;

  // API mesh shader entry block
  BasicBlock *beginMeshShaderBlock = &entryPoint->getEntryBlock();
  beginMeshShaderBlock->setName(".beginMeshShader");

  // API mesh shader exit block
  BasicBlock *retBlock = nullptr;
  for (auto &block : *entryPoint) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retBlock = &block;
      break;
    }
  }
  assert(retBlock);
  auto endMeshShaderBlock = retBlock->splitBasicBlock(retBlock->getTerminator(), ".endMeshShader");

  // Helper to create basic block
  auto createBlock = [&](const char *blockName, BasicBlock *insertBefore = nullptr) {
    return BasicBlock::Create(entryPoint->getParent()->getContext(), blockName, entryPoint, insertBefore);
  };

  auto entryBlock = createBlock(".entry", beginMeshShaderBlock);
  auto initPrimitiveIndicesHeaderBlock = createBlock(".initPrimitiveIndicesHeader", beginMeshShaderBlock);
  auto initPrimitiveIndicesBodyBlock = createBlock(".initPrimitiveIndicesBody", beginMeshShaderBlock);
  auto endInitPrimitiveIndicesBlock = createBlock(".endInitPrimitiveIndices", beginMeshShaderBlock);

  auto writeSpecialValueBlock = createBlock(".writeSpecialValue", beginMeshShaderBlock);
  auto endWriteSpecialValueBlock = createBlock(".endWriteSpecialValue", beginMeshShaderBlock);

  auto checkDummyAllocReqBlock = createBlock(".checkDummyAllocReq");
  auto dummyAllocReqBlock = createBlock(".dummyAllocReq");
  auto endDummyAllocReqBlock = createBlock(".endDummyAllocReq");
  auto checkExportPrimitiveBlock = createBlock(".checkExportPrimitive");

  auto exportPrimitiveHeaderBlock = createBlock(".exportPrimitiveHeader");
  auto exportPrimitiveBodyBlock = createBlock(".exportPrimitiveBody");
  auto endExportPrimitiveBlock = createBlock(".endExportPrimitive");

  auto exportVertexHeaderBlock = createBlock(".exportVertexHeader");
  auto exportVertexBodyBlock = createBlock(".exportVertexBody");
  auto endExportVertexBlock = createBlock(".endExportVertex");

  auto collectMeshStatsBlock = createBlock(".collectMeshStats");
  auto exitBlock = createBlock(".exit");

  // Construct ".entry" block
  {
    m_builder->SetInsertPoint(entryBlock);

    initWaveThreadInfo(entryPoint);

    m_builder->CreateBr(initPrimitiveIndicesHeaderBlock);
  }

  // Construct ".initPrimitiveIndicesHeader" block
  PHINode *loopIndexPhi = nullptr;
  {
    m_builder->SetInsertPoint(initPrimitiveIndicesHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder->getInt32(0), entryBlock); // loopIndex = 0

      // primitiveIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder->CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                               m_builder->CreateMul(loopIndexPhi, m_builder->getInt32(waveSize)), "primitiveIndex");
    }

    auto validPrimitive =
        m_builder->CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, m_builder->getInt32(meshMode.outputPrimitives));
    m_builder->CreateCondBr(validPrimitive, initPrimitiveIndicesBodyBlock, endInitPrimitiveIndicesBlock);
  }

  // Construct ".initPrimitiveIndicesBody" block
  {
    m_builder->SetInsertPoint(initPrimitiveIndicesBodyBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   primitiveIndex = threadIdInSubgroup
      //
      //   while (primitiveIndex < outputPrimitives) {
      //     Zero primitive connectivity data
      //
      //     loopIndex += numWaves
      //     primitiveIndex += loopIndex * waveSize
      //   }
      //
      auto loopIndex = m_builder->CreateAdd(loopIndexPhi, m_builder->getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, initPrimitiveIndicesBodyBlock);
    }

    auto ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
    auto ldsOffset = m_builder->CreateAdd(ldsStart, m_waveThreadInfo.primOrVertexIndex);

    writeValueToLds(m_builder->getInt32(0), ldsOffset);
    m_builder->CreateBr(m_pipelineState->enableMeshRowExport() ? initPrimitiveIndicesHeaderBlock
                                                               : endInitPrimitiveIndicesBlock);
  }

  // Construct ".endInitPrimitiveIndices" block
  Value *firstThreadInSubgroup = nullptr;
  {
    m_builder->SetInsertPoint(endInitPrimitiveIndicesBlock);

    firstThreadInSubgroup = m_builder->CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstThreadInSubgroup, writeSpecialValueBlock, endWriteSpecialValueBlock);
  }

  // Construct ".writeSpecialValue" block
  {
    m_builder->SetInsertPoint(writeSpecialValueBlock);

    // NOTE: We write invalid value (~0) to vertex count as the sentinel. If API mesh shader executes
    // SetMeshOutputs, the value will be changed to a valid one. Otherwise, we know SetMeshOutputs is not be
    // executed and we must make a dummy sendmsg (GS_ALLOC_REQ) with zero vertex/primitive count.
    auto ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    writeValueToLds(m_builder->getInt32(InvalidValue), ldsOffset);

    // Write flat workgroup ID to LDS if it is required. Otherwise, skip it.
    if (useFlatWorkgroupId(m_pipelineState)) {
      auto ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::FlatWorkgroupId));
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
      auto flatWorkgroupId = getFunctionArgument(entryPoint, entryArgIdxs.flatWorkgroupId);
      writeValueToLds(flatWorkgroupId, ldsOffset);
    }

    m_builder->CreateBr(endWriteSpecialValueBlock);
  }

  // Construct ".endWriteSpecialValue" block
  {
    m_builder->SetInsertPoint(endWriteSpecialValueBlock);

    SyncScope::ID syncScope = entryPoint->getParent()->getContext().getOrInsertSyncScopeID("workgroup");
    m_builder->CreateFence(AtomicOrdering::Release, syncScope);
    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    m_builder->CreateFence(AtomicOrdering::Acquire, syncScope);

    auto validMesh = m_builder->CreateICmpULT(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(numMeshThreads));
    m_builder->CreateCondBr(validMesh, beginMeshShaderBlock, endMeshShaderBlock);
  }

  // Lower mesh shader main body
  lowerMeshShaderBody(beginMeshShaderBlock);

  // Construct ".endMeshShader" block
  Value *vertexCount = nullptr;
  Value *primitiveCount = nullptr;
  {
    m_builder->SetInsertPoint(endMeshShaderBlock);

    // NOTE: Here, we remove original return instruction from API mesh shader and continue to construct this block
    // with other instructions.
    endMeshShaderBlock->getTerminator()->eraseFromParent();

    SyncScope::ID syncScope = entryPoint->getParent()->getContext().getOrInsertSyncScopeID("workgroup");
    m_builder->CreateFence(AtomicOrdering::Release, syncScope);
    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    m_builder->CreateFence(AtomicOrdering::Acquire, syncScope);

    Value *ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    vertexCount = readValueFromLds(m_builder->getInt32Ty(), ldsOffset);
    vertexCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertexCount); // Promoted to SGPR
    vertexCount->setName("vertexCount");

    ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveCount));
    primitiveCount = readValueFromLds(m_builder->getInt32Ty(), ldsOffset);
    primitiveCount =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primitiveCount); // Promoted to SGPR
    primitiveCount->setName("primitiveCount");

    auto dummyAllocReq = m_builder->CreateICmpEQ(vertexCount, m_builder->getInt32(InvalidValue));
    m_builder->CreateCondBr(dummyAllocReq, checkDummyAllocReqBlock, checkExportPrimitiveBlock);
  }

  // Construct ".checkDummyAllocReq" block
  {
    m_builder->SetInsertPoint(checkDummyAllocReqBlock);

    m_builder->CreateCondBr(firstThreadInSubgroup, dummyAllocReqBlock, endDummyAllocReqBlock);
  }

  // Construct ".dummyAllocReq" block
  {
    m_builder->SetInsertPoint(dummyAllocReqBlock);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {},
                               {m_builder->getInt32(GsAllocReq), m_builder->getInt32(0)});
    m_builder->CreateBr(endDummyAllocReqBlock);
  }

  // Construct ".endDummyAllocReq" block
  {
    m_builder->SetInsertPoint(endDummyAllocReqBlock);

    m_builder->CreateRetVoid();
  }

  // Construct ".checkExportPrimitive" block
  {
    m_builder->SetInsertPoint(checkExportPrimitiveBlock);

    m_builder->CreateBr(exportPrimitiveHeaderBlock);
  }

  // Construct ".exportPrimitiveHeader" block
  {
    m_builder->SetInsertPoint(exportPrimitiveHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder->getInt32(0), checkExportPrimitiveBlock); // loopIndex = 0

      // primitiveIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder->CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                               m_builder->CreateMul(loopIndexPhi, m_builder->getInt32(waveSize)), "primitiveIndex");
    }

    auto validPrimitive = m_builder->CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, primitiveCount);
    m_builder->CreateCondBr(validPrimitive, exportPrimitiveBodyBlock, endExportPrimitiveBlock);
  }

  // Construct ".exportPrimitiveBody" block
  {
    m_builder->SetInsertPoint(exportPrimitiveBodyBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   primitiveIndex = threadIdInSubgroup
      //
      //   while (primitiveIndex < primitiveCount) {
      //     Export primitive
      //     Export primitive attributes
      //
      //     loopIndex += numWaves
      //     primitiveIndex += loopIndex * waveSize
      //   }
      //
      auto loopIndex = m_builder->CreateAdd(loopIndexPhi, m_builder->getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportPrimitiveBodyBlock);
    }

    exportPrimitive();
    m_builder->CreateBr(m_pipelineState->enableMeshRowExport() ? exportPrimitiveHeaderBlock : endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder->SetInsertPoint(endExportPrimitiveBlock);

    m_builder->CreateBr(exportVertexHeaderBlock);
  }

  // Construct ".exportVertexHeader" block
  {
    m_builder->SetInsertPoint(exportVertexHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder->getInt32(0), endExportPrimitiveBlock); // loopIndex = 0

      // vertexIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder->CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                               m_builder->CreateMul(loopIndexPhi, m_builder->getInt32(waveSize)), "vertexIndex");
    }

    auto validVertex = m_builder->CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, vertexCount);
    m_builder->CreateCondBr(validVertex, exportVertexBodyBlock, endExportVertexBlock);
  }

  // Construct "exportVertexBody" block
  {
    m_builder->SetInsertPoint(exportVertexBodyBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   vertexIndex = threadIdInSubgroup
      //
      //   while (vertexIndex < vertexCount) {
      //     Export vertex position data
      //     Export vertex attributes
      //
      //     loopIndex += numWaves
      //     vertexIndex += loopIndex * waveSize
      //   }
      //
      auto loopIndex = m_builder->CreateAdd(loopIndexPhi, m_builder->getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportVertexBodyBlock);
    }

    exportVertex();
    m_builder->CreateBr(m_pipelineState->enableMeshRowExport() ? exportVertexHeaderBlock : endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder->SetInsertPoint(endExportVertexBlock);

    auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstThreadInSubgroup, collectMeshStatsBlock, exitBlock);
  }

  // Construct ".collectMeshStats" block
  {
    m_builder->SetInsertPoint(collectMeshStatsBlock);

    collectMeshStatsInfo(entryPoint, primitiveCount);
    m_builder->CreateBr(exitBlock);
  }

  // Construct ".exit" block
  {
    m_builder->SetInsertPoint(exitBlock);

    m_builder->CreateRetVoid();
  }
}

// =====================================================================================================================
// Process the read of task payload.
//
// @param readTy : Type of value to read
// @param byteOffset : Byte offset within the payload entry
// @returns : Value read from task payload
Value *MeshTaskShader::readTaskPayload(Type *readTy, Value *byteOffset) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  CoherentFlag coherent = {};
  coherent.bits.glc = true;
  coherent.bits.dlc = true;

  const unsigned bitWidth = readTy->getScalarSizeInBits();
  const unsigned numElements = readTy->isVectorTy() ? cast<FixedVectorType>(readTy)->getNumElements() : 1;
  assert(numElements >= 1 && numElements <= 4);

  // NOTE: There are some special types that LLVM backend couldn't support. We have to lower them here.
  if (bitWidth == 64) {
    // 64 -> vec2
    // 64vec2 -> vec4
    // 64vec3 -> vec4 + vec2
    // 64vec4 -> vec4 + vec4
    Type *readTy1 = FixedVectorType::get(m_builder->getInt32Ty(), std::min(2 * numElements, 4u));
    Value *readValue1 = readTaskPayload(readTy1, byteOffset);

    Value *readValue = nullptr;
    if (numElements > 2) {
      Type *readTy2 = FixedVectorType::get(m_builder->getInt32Ty(), 2 * numElements - 4);
      byteOffset = m_builder->CreateAdd(byteOffset, m_builder->getInt32(4 * sizeof(unsigned)));
      Value *readValue2 = readTaskPayload(readTy2, byteOffset);

      if (numElements == 3) {
        readValue2 = m_builder->CreateShuffleVector(readValue2, PoisonValue::get(readValue2->getType()),
                                                    ArrayRef<int>{0, 1, 2, 3});
      }
      readValue = m_builder->CreateShuffleVector(readValue1, readValue2,
                                                 ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7}.slice(0, numElements * 2));
    } else {
      readValue = readValue1;
    }

    return m_builder->CreateBitCast(readValue, readTy);
  } else if (bitWidth == 8 || bitWidth == 16) {
    if (numElements > 1) {
      // Scalarize
      Value *readValue = UndefValue::get(readTy);
      for (unsigned i = 0; i < numElements; ++i) {
        auto elemByteOffset =
            i > 0 ? m_builder->CreateAdd(byteOffset, m_builder->getInt32(i * bitWidth / 8)) : byteOffset;
        auto elem = readTaskPayload(readTy->getScalarType(), elemByteOffset);
        readValue = m_builder->CreateInsertElement(readValue, elem, i);
      }
      return readValue;
    }
  }

  return m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_raw_buffer_load, readTy,
      {payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(coherent.u32All)});
}

// =====================================================================================================================
// Process the write of task payload.
//
// @param writeValue : Value to write
// @param byteOffset : Byte offset within the payload entry
void MeshTaskShader::writeTaskPayload(Value *writeValue, Value *byteOffset) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  CoherentFlag coherent = {};
  coherent.bits.glc = true;

  const auto writeTy = writeValue->getType();
  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  const unsigned numElements = writeTy->isVectorTy() ? cast<FixedVectorType>(writeTy)->getNumElements() : 1;
  assert(numElements >= 1 && numElements <= 4);

  // NOTE: There are some special types that LLVM backend couldn't support. We have to lower them here.
  if (bitWidth == 64) {
    // Cast to <n x i32>
    auto castTy = FixedVectorType::get(m_builder->getInt32Ty(), 2 * numElements);
    writeValue = m_builder->CreateBitCast(writeValue, castTy);

    // 64scalar -> vec2
    // 64vec2 -> vec4
    // 64vec3 -> vec4 + vec2
    // 64vec4 -> vec4 + vec4
    auto writeValue1 = writeValue;
    if (numElements > 2) {
      writeValue1 = m_builder->CreateShuffleVector(writeValue, PoisonValue::get(writeValue->getType()),
                                                   ArrayRef<int>({0, 1, 2, 3}));
    }
    writeTaskPayload(writeValue1, byteOffset);

    if (numElements > 2) {
      auto writeValue2 = m_builder->CreateShuffleVector(writeValue, PoisonValue::get(writeValue->getType()),
                                                        ArrayRef<int>({4, 5, 6, 7}).slice(0, 2 * numElements - 4));
      byteOffset = m_builder->CreateAdd(byteOffset, m_builder->getInt32(4 * sizeof(unsigned)));
      writeTaskPayload(writeValue2, byteOffset);
    }

    return;
  } else if (bitWidth == 8 || bitWidth == 16) {
    if (numElements > 1) {
      // Scalarize
      for (unsigned i = 0; i < numElements; ++i) {
        auto elem = m_builder->CreateExtractElement(writeValue, i);
        auto elemByteOffset =
            i > 0 ? m_builder->CreateAdd(byteOffset, m_builder->getInt32(i * bitWidth / 8)) : byteOffset;
        writeTaskPayload(elem, elemByteOffset);
      }
      return;
    }
  }

  m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_raw_buffer_store, writeValue->getType(),
      {writeValue, payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(coherent.u32All)});
}

// =====================================================================================================================
// Create a task payload atomic operation other than compare-and-swap. Result type is the same as the input value type.
//
// @param atomicOp : Atomic op to perform
// @param ordering : Atomic ordering
// @param inputValue : Input value
// @param byteOffset : Byte offset within the payload structure
// @returns : Original value read from the task payload
Value *MeshTaskShader::taskPayloadAtomic(unsigned atomicOp, AtomicOrdering ordering, Value *inputValue,
                                         Value *byteOffset) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  assert(inputValue->getType()->isIntegerTy() || inputValue->getType()->isFloatingPointTy());

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  SyncScope::ID syncScope = entryPoint->getParent()->getContext().getOrInsertSyncScopeID("workgroup");

  // NOTE: buffer.atomic.swap.f64 is not supported in LLVM backend, so we convert double to int64.
  bool doubleToInt64 = atomicOp == AtomicRMWInst::Xchg && inputValue->getType()->isDoubleTy();
  if (doubleToInt64)
    inputValue = m_builder->CreateBitCast(inputValue, m_builder->getInt64Ty());

  Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;
  switch (atomicOp) {
  case AtomicRMWInst::Xchg:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_swap;
    break;
  case AtomicRMWInst::Add:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_add;
    break;
  case AtomicRMWInst::Sub:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_sub;
    break;
  case AtomicRMWInst::And:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_and;
    break;
  case AtomicRMWInst::Or:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_or;
    break;
  case AtomicRMWInst::Xor:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_xor;
    break;
  case AtomicRMWInst::Max:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_smax;
    break;
  case AtomicRMWInst::Min:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_smin;
    break;
  case AtomicRMWInst::UMax:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_umax;
    break;
  case AtomicRMWInst::UMin:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_umin;
    break;
  case AtomicRMWInst::FAdd:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fadd;
    break;
  case AtomicRMWInst::FMax:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fmax;
    break;
  case AtomicRMWInst::FMin:
    intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fmin;
    break;
  default:
    llvm_unreachable("Unexpected atomic operation!");
    break;
  }

  if (ordering == AtomicOrdering::Release || ordering == AtomicOrdering::AcquireRelease ||
      ordering == AtomicOrdering::SequentiallyConsistent)
    m_builder->CreateFence(AtomicOrdering::Release, syncScope);

  Value *atomicCall = m_builder->CreateIntrinsic(
      intrinsic, inputValue->getType(),
      {inputValue, payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(0)});

  if (doubleToInt64)
    atomicCall = m_builder->CreateBitCast(atomicCall, m_builder->getDoubleTy());

  if (ordering == AtomicOrdering::Release || ordering == AtomicOrdering::AcquireRelease ||
      ordering == AtomicOrdering::SequentiallyConsistent)
    m_builder->CreateFence(AtomicOrdering::Acquire, syncScope);

  return atomicCall;
}

// =====================================================================================================================
// Create a task payload atomic compare-and-swap.
//
// @param ordering : Atomic ordering
// @param inputValue : Input value
// @param comparatorValue : Value to compare against
// @param byteOffset : Byte offset within the payload structure
// @returns : Original value read from the task payload
Value *MeshTaskShader::taskPayloadAtomicCompareSwap(AtomicOrdering ordering, Value *inputValue, Value *comparatorValue,
                                                    Value *byteOffset) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  assert(inputValue->getType()->isIntegerTy() || inputValue->getType()->isFloatingPointTy());

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  SyncScope::ID syncScope = entryPoint->getParent()->getContext().getOrInsertSyncScopeID("workgroup");

  if (inputValue->getType()->isIntegerTy(64)) {
    // NOTE: HW doesn't have buffer_atomic_cmpswap_x2 instruction, we resort to global_atomic_cmpswap_x2.

    // 48-bit GPU address of from the buffer descriptor: dword1[15:0] + dword0
    auto baseAddressLow = m_builder->CreateExtractElement(payloadRingBufDesc, static_cast<uint64_t>(0));
    auto baseAddressHigh = m_builder->CreateExtractElement(payloadRingBufDesc, 1);
    baseAddressHigh = m_builder->CreateAnd(baseAddressHigh, 0xFFFF);

    Value *baseAddress = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
    baseAddress = m_builder->CreateInsertElement(baseAddress, baseAddressLow, static_cast<uint64_t>(0));
    baseAddress = m_builder->CreateInsertElement(baseAddress, baseAddressHigh, 1);
    baseAddress = m_builder->CreateBitCast(baseAddress, m_builder->getInt64Ty());

    Value *payloadRingBufPtr = m_builder->CreateIntToPtr(baseAddress, m_builder->getInt8PtrTy(ADDR_SPACE_GLOBAL));
    Value *entryOffset = m_builder->CreateAdd(payloadRingEntryOffset, byteOffset);
    Value *payloadRingBufEntryPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), payloadRingBufPtr, entryOffset);
    payloadRingBufEntryPtr =
        m_builder->CreateBitCast(payloadRingBufEntryPtr, PointerType::get(m_builder->getInt64Ty(), ADDR_SPACE_GLOBAL));

    auto atomicInst = m_builder->CreateAtomicCmpXchg(payloadRingBufEntryPtr, comparatorValue, inputValue, MaybeAlign(),
                                                     ordering, AtomicOrdering::Monotonic, syncScope);
    // NOTE: In cmpxchg instruction in LLVM returns a structure-typed result {<value>, i1}, we don't care about the
    // second member.
    return m_builder->CreateExtractValue(atomicInst, 0);
  }

  if (ordering == AtomicOrdering::Release || ordering == AtomicOrdering::AcquireRelease ||
      ordering == AtomicOrdering::SequentiallyConsistent)
    m_builder->CreateFence(AtomicOrdering::Release, syncScope);

  Value *atomicCall = m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_raw_buffer_atomic_cmpswap, inputValue->getType(),
      {inputValue, comparatorValue, payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(0)});

  if (ordering == AtomicOrdering::Release || ordering == AtomicOrdering::AcquireRelease ||
      ordering == AtomicOrdering::SequentiallyConsistent)
    m_builder->CreateFence(AtomicOrdering::Acquire, syncScope);

  return atomicCall;
}

// =====================================================================================================================
// Initialize the wave/thread info from the entry-point.
//
// @param entryPoint : Shader entry-point
void MeshTaskShader::initWaveThreadInfo(Function *entryPoint) {
  m_waveThreadInfo = {}; // Reset it

  if (getShaderStage(entryPoint) == ShaderStageTask) {
    // Task shader
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTask)->entryArgIdxs.task;

    // waveId = dispatchInfo[24:20]
    m_waveThreadInfo.waveIdInSubgroup =
        m_builder->CreateAnd(m_builder->CreateLShr(getFunctionArgument(entryPoint, entryArgIdxs.multiDispatchInfo), 20),
                             0x1F, "waveIdInSubgroup");

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTask);

    m_waveThreadInfo.threadIdInWave =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder->getInt32(-1), m_builder->getInt32(0)});
    if (waveSize == 64) {
      m_waveThreadInfo.threadIdInWave = m_builder->CreateIntrinsic(
          Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder->getInt32(-1), m_waveThreadInfo.threadIdInWave});
    }
    m_waveThreadInfo.threadIdInWave->setName("threadIdInWave");

    m_waveThreadInfo.threadIdInSubgroup =
        m_builder->CreateAdd(m_builder->CreateMul(m_waveThreadInfo.waveIdInSubgroup, m_builder->getInt32(waveSize)),
                             m_waveThreadInfo.threadIdInWave, "threadIdInSubgroup");
  } else {
    // Mesh shader
    assert(getShaderStage(entryPoint) == ShaderStageMesh);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder->getInt64(-1));

    // waveId = mergedWaveInfo[27:24]
    Value *mergedWaveInfo =
        getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
    m_waveThreadInfo.waveIdInSubgroup =
        m_builder->CreateAnd(m_builder->CreateLShr(mergedWaveInfo, 24), 0xF, "waveIdInSubgroup");

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageMesh);

    m_waveThreadInfo.threadIdInWave =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder->getInt32(-1), m_builder->getInt32(0)});
    if (waveSize == 64) {
      m_waveThreadInfo.threadIdInWave = m_builder->CreateIntrinsic(
          Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder->getInt32(-1), m_waveThreadInfo.threadIdInWave});
    }
    m_waveThreadInfo.threadIdInWave->setName("threadIdInWave");

    m_waveThreadInfo.threadIdInSubgroup =
        m_builder->CreateAdd(m_builder->CreateMul(m_waveThreadInfo.waveIdInSubgroup, m_builder->getInt32(waveSize)),
                             m_waveThreadInfo.threadIdInWave, "threadIdInSubgroup");

    m_waveThreadInfo.primOrVertexIndex =
        m_waveThreadInfo.threadIdInSubgroup; // Primitive or vertex index is initialized to thread ID in subgroup
  }
}

// =====================================================================================================================
// Get shader ring entry index of current workgroup from the entry-point.
//
// @param entryPoint : Shader entry-point
// @returns : The shader ring entry index of current workgroup
Value *MeshTaskShader::getShaderRingEntryIndex(Function *entryPoint) {
  if (!m_shaderRingEntryIndex) {
    if (getShaderStage(entryPoint) == ShaderStageTask) {
      // NOTE: The calculation of shader ring entry index should be done at the beginning of the entry block. And the
      // value could be reused in subsequent operations.
      IRBuilder<>::InsertPointGuard guard(*m_builder);
      m_builder->SetInsertPointPastAllocas(entryPoint);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTask)->entryArgIdxs.task;

      auto workgroupId = getFunctionArgument(entryPoint, entryArgIdxs.workgroupId);
      auto dispatchDims = getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);

      // flatWorkgroupId = workgroupId.z * dispatchDims.x * dispatchDims.y +
      //                   workgroupId.y * dispatchDims.x + workgroupId.x
      //                 = (workgroupId.z * dispatchDims.y + workgroupId.y) * dispatchDims.x + workgroupId.x
      auto flatWorkgroupId = m_builder->CreateMul(m_builder->CreateExtractElement(workgroupId, 2),
                                                  m_builder->CreateExtractElement(dispatchDims, 1));
      flatWorkgroupId = m_builder->CreateAdd(flatWorkgroupId, m_builder->CreateExtractElement(workgroupId, 1));
      flatWorkgroupId = m_builder->CreateMul(flatWorkgroupId,
                                             m_builder->CreateExtractElement(dispatchDims, static_cast<uint64_t>(0)));
      flatWorkgroupId =
          m_builder->CreateAdd(flatWorkgroupId, m_builder->CreateExtractElement(workgroupId, static_cast<uint64_t>(0)));

      auto baseRingEntryIndex = getFunctionArgument(entryPoint, entryArgIdxs.baseRingEntryIndex);
      m_shaderRingEntryIndex = m_builder->CreateAdd(baseRingEntryIndex, flatWorkgroupId);
    } else {
      assert(getShaderStage(entryPoint) == ShaderStageMesh);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
      m_shaderRingEntryIndex = getFunctionArgument(entryPoint, entryArgIdxs.baseRingEntryIndex);
    }
  }

  return m_shaderRingEntryIndex;
}

// =====================================================================================================================
// Get the payload ring entry offset of current workgroup for task shader.
//
// @param entryPoint : Entry-point of task shader
// @returns : The payload ring entry offset of current workgroup
Value *MeshTaskShader::getPayloadRingEntryOffset(Function *entryPoint) {
  if (!m_payloadRingEntryOffset) {
    Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
    Value *payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();

    // NOTE: Make sure below calculation follows payload ring descriptor getter and is prior to any task payload
    // access operations.
    IRBuilder<>::InsertPointGuard guard(*m_builder);
    m_builder->SetInsertPoint(cast<Instruction>(payloadRingBufDesc)->getNextNode());

    // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
    Value *numPayloadRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(payloadRingBufDesc, 2),
                                                         m_builder->getInt32(PayloadRingEntrySize));
    // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
    Value *wrappedRingEntryIndex =
        m_builder->CreateAnd(ringEntryIndex, m_builder->CreateSub(numPayloadRingEntries, m_builder->getInt32(1)));
    m_payloadRingEntryOffset = m_builder->CreateMul(wrappedRingEntryIndex, m_builder->getInt32(PayloadRingEntrySize));
  }

  return m_payloadRingEntryOffset;
}

// =====================================================================================================================
// Get the draw data ring entry offset of current workgroup for task shader.
//
// @param entryPoint : Entry-point of task shader
// @returns : The draw data ring entry offset of current workgroup
Value *MeshTaskShader::getDrawDataRingEntryOffset(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
  Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();

  // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
  Value *numDrawDataRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(drawDataRingBufDesc, 2),
                                                        m_builder->getInt32(DrawDataRingEntrySize));
  // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
  Value *wrappedRingEntryIndex =
      m_builder->CreateAnd(ringEntryIndex, m_builder->CreateSub(numDrawDataRingEntries, m_builder->getInt32(1)));
  return m_builder->CreateMul(wrappedRingEntryIndex, m_builder->getInt32(DrawDataRingEntrySize));
}

// =====================================================================================================================
// Get the draw data ready bit.
//
// @param entryPoint : Entry-point of task shader
// @returns : Flag (i1 typed) indicating whether the draw data is ready for command processor (CP) to fetch.
Value *MeshTaskShader::getDrawDataReadyBit(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
  Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();

  // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
  Value *numDrawDataRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(drawDataRingBufDesc, 2),
                                                        m_builder->getInt32(DrawDataRingEntrySize));
  // readyBit = ringEntryIndex & numRingEnties != 0
  return m_builder->CreateICmpNE(m_builder->CreateAnd(ringEntryIndex, numDrawDataRingEntries), m_builder->getInt32(0));
}

// =====================================================================================================================
// Emit mesh tasks. Defines the dimension size of subsequent mesh shader workgroups to generate upon completion of the
// task shader workgroup
//
// @param groupCountX : Number of local workgroups in X dimension for the launch of child mesh tasks
// @param groupCountX : Number of local workgroups in Y dimension for the launch of child mesh tasks
// @param groupCountX : Number of local workgroups in Z dimension for the launch of child mesh tasks
void MeshTaskShader::emitTaskMeshs(Value *groupCountX, Value *groupCountY, Value *groupCountZ) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  auto emitMeshsCall = m_builder->GetInsertPoint();

  auto checkEmitMeshsBlock = m_builder->GetInsertBlock();
  auto emitMeshsBlock = checkEmitMeshsBlock->splitBasicBlock(emitMeshsCall, ".emitMeshs");
  auto endEmitMeshsBlock = emitMeshsBlock->splitBasicBlock(emitMeshsCall, ".endEmitMeshs");

  // Modify ".checkEmitMeshs" block
  {
    m_builder->SetInsertPoint(checkEmitMeshsBlock->getTerminator());

    if (m_accessTaskPayload) {
      // Make sure the task payload read/write access is completed
      m_builder->CreateFence(AtomicOrdering::Release, SyncScope::System);
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    }

    auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstThreadInSubgroup, emitMeshsBlock, endEmitMeshsBlock);
    checkEmitMeshsBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".emitTaskMeshs" block
  {
    m_builder->SetInsertPoint(emitMeshsBlock->getTerminator());

    //
    // Collect task statistics info
    //
    if (m_pipelineState->needSwMeshPipelineStats()) {
      auto &computeMode =
          m_pipelineState->getShaderModes()->getComputeShaderMode(); // Task shader is actually a compute shader
      const uint64_t numTaskThreads =
          computeMode.workgroupSizeX * computeMode.workgroupSizeY * computeMode.workgroupSizeZ;

      Value *meshPipeStatsBufPtr = m_pipelineSysValues.get(entryPoint)->getMeshPipeStatsBufPtr();
      Value *meshPipeStatsBufEntryPtr =
          m_builder->CreateGEP(m_builder->getInt8Ty(), meshPipeStatsBufPtr,
                               m_builder->getInt32(offsetof(MeshPipeStatsEntry, numTaskThreads)));
      meshPipeStatsBufEntryPtr = m_builder->CreateBitCast(meshPipeStatsBufEntryPtr,
                                                          PointerType::get(m_builder->getInt64Ty(), ADDR_SPACE_GLOBAL));

      // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
      // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
      // move the added value to VGPR to mark it as "divergent".
      Value *valueToAdd = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
      valueToAdd = m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numTaskThreads)),
                                                  static_cast<uint64_t>(0));
      valueToAdd =
          m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numTaskThreads >> 32)), 1);
      valueToAdd = m_builder->CreateBitCast(valueToAdd, m_builder->getInt64Ty());

      m_builder->CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                                 AtomicOrdering::Monotonic, SyncScope::System);
    }

    //
    // Write draw data
    //

    // Set X dimension to 0 if any of X, Y, Z dimension is 0:
    //   groupCountX = min(groupCountY, groupCountZ) == 0 ? 0 : groupCountX
    auto minGroupCountYZ =
        m_builder->CreateIntrinsic(Intrinsic::umin, groupCountY->getType(), {groupCountY, groupCountZ});
    groupCountX = m_builder->CreateSelect(m_builder->CreateICmpEQ(minGroupCountYZ, m_builder->getInt32(0)),
                                          m_builder->getInt32(0), groupCountX);

    Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();
    Value *drawDataRingEntryOffset = getDrawDataRingEntryOffset(entryPoint);

    // Draw data (<4 x i32>) = <groupCountX, groupCountY, groupCountZ, readyBit>
    Value *drawData = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 4));
    drawData = m_builder->CreateInsertElement(drawData, groupCountX, static_cast<uint64_t>(0));
    drawData = m_builder->CreateInsertElement(drawData, groupCountY, 1);
    drawData = m_builder->CreateInsertElement(drawData, groupCountZ, 2);

    Value *readyBit = getDrawDataReadyBit(entryPoint);
    drawData = m_builder->CreateInsertElement(drawData, m_builder->CreateZExt(readyBit, m_builder->getInt32Ty()), 3);

    m_builder->CreateIntrinsic(
        Intrinsic::amdgcn_raw_buffer_store, drawData->getType(),
        {drawData, drawDataRingBufDesc, m_builder->getInt32(0), drawDataRingEntryOffset, m_builder->getInt32(0)});
  }

  // Construct ".endEmitTaskMeshs" block
  {
    m_builder->SetInsertPoint(endEmitMeshsBlock->getTerminator());

    // Currently, nothing to do
  }
}

// =====================================================================================================================
// Convert a i32 value to divergent one by inserting a "v_mov_b32" forcibly.
//
// @param value : Input i32 value
// @returns : A new i32 value that is considered to be divergent
Value *MeshTaskShader::convertToDivergent(Value *value) {
  assert(value->getType() == m_builder->getInt32Ty()); // Must be i32 typed
  auto inlineAsmTy = FunctionType::get(m_builder->getInt32Ty(), m_builder->getInt32Ty(), false);
  auto inlineAsm = InlineAsm::get(inlineAsmTy, "v_mov_b32 $0, $1", "=v,0", true);
  return m_builder->CreateCall(inlineAsm, value);
}

// =====================================================================================================================
// Mutate entry-point of mesh shader by adding SGPR amd VGPR shader inputs. The system GPR layout is based on the
// requirements of HW GS since mesh shader is mapped to HW GS in fast launch mode.
//
// @param entryPoint : Entry-point of mesh shader
// @returns : New entry-point of mesh shader after mutation
Function *MeshTaskShader::mutateMeshShaderEntryPoint(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  // GFX10 special SGPR input names
  static const SmallVector<std::string, NumSpecialSgprInputs> SpecialSgprInputNamesGfx10 = {
      "gsUserDataAddrLow", "gsUserDataAddrHigh",  "mergedGroupInfo", "mergedWaveInfo",
      "offChipLdsBase",    "sharedScratchOffset", "gsShaderAddrLow", "gsShaderAddrHigh",
  };

  ArrayRef<std::string> specialSgprInputNames;
  if (m_gfxIp.major == 10)
    specialSgprInputNames = makeArrayRef(SpecialSgprInputNamesGfx10);
  assert(specialSgprInputNames.size() == NumSpecialSgprInputs);

  // Add special SGPR inputs, prior to existing user data SGPRs
  auto int32Ty = m_builder->getInt32Ty();
  auto newEntryPoint =
      addFunctionArgs(entryPoint, nullptr, {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty},
                      specialSgprInputNames, (1u << NumSpecialSgprInputs) - 1);

  assert(entryPoint->use_empty());
  entryPoint->eraseFromParent();

  // Adjust indices of existing entry-point arguments
  auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
  entryArgIdx.drawIndex += NumSpecialSgprInputs;
  entryArgIdx.viewIndex += NumSpecialSgprInputs;
  entryArgIdx.dispatchDims += NumSpecialSgprInputs;
  entryArgIdx.baseRingEntryIndex += NumSpecialSgprInputs;
  entryArgIdx.pipeStatsBuf += NumSpecialSgprInputs;

  // NOTE: If flat workgroup ID is required, we have to add HW GS VGPRs. Only the VGPR5 "vertexId" will be used to
  // emulate flat workgroup ID since HW GS is configurated to have one vertex and one primitive in one input thread.
  // The "vertexId" VGPR5 will be incremented by 1 for each subgroup.
  if (useFlatWorkgroupId(m_pipelineState)) {
    static const SmallVector<std::string, 6> VgprInputNames = {"esGsOffset01", "esGsOffset23", "gsPrimitiveId",
                                                               "gsInstanceId", "esGsOffset45", "flatWorkgroupId"};

    entryPoint = newEntryPoint;
    newEntryPoint = addFunctionArgs(entryPoint, nullptr, {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty},
                                    VgprInputNames, 0, true);

    assert(entryPoint->use_empty());
    entryPoint->eraseFromParent();

    entryArgIdx.flatWorkgroupId = newEntryPoint->arg_size() - 1; // The last argument
  }

  return newEntryPoint;
}

// =====================================================================================================================
// Lower mesh shader main body by lowering mesh shader specific calls.
//
// @param beginMeshShaderBlock : API mesh shader entry block (before any mutation)
void MeshTaskShader::lowerMeshShaderBody(BasicBlock *beginMeshShaderBlock) {
  auto entryPoint = beginMeshShaderBlock->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  SmallVector<CallInst *, 8> removedCalls;

  // Lower mesh shader calls
  auto module = entryPoint->getParent();
  for (auto &func : module->functions()) {
    if (!func.isDeclaration())
      continue; // Not targets

    if (func.getName().startswith(lgcName::MeshTaskCallPrefix)) {
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);

        if (call->getFunction() != entryPoint)
          continue; // Not belong to mesh shader

        m_builder->SetInsertPoint(call);

        if (func.getName().startswith(lgcName::MeshTaskSetMeshOutputs)) {
          // Set mesh outputs
          assert(call->arg_size() == 2);
          auto vertexCount = call->getOperand(0);
          auto primitiveCount = call->getOperand(1);

          setMeshOutputs(vertexCount, primitiveCount);
        } else if (func.getName().startswith(lgcName::MeshTaskSetPrimitiveIndices)) {
          // Set primitive indices
          assert(call->arg_size() == 2);
          auto primitiveIndex = call->getOperand(0);
          auto primitiveIndices = call->getOperand(1);

          setPrimitiveIndices(primitiveIndex, primitiveIndices);
        } else if (func.getName().startswith(lgcName::MeshTaskSetPrimitiveCulled)) {
          // Set primitive culled
          assert(call->arg_size() == 2);
          auto primitiveIndex = call->getOperand(0);
          auto isCulled = call->getOperand(1);

          setPrimitiveCulled(primitiveIndex, isCulled);
        } else if (func.getName().startswith(lgcName::MeshTaskGetMeshInput)) {
          // Get mesh input
          assert(call->arg_size() == 1);
          unsigned builtIn = cast<ConstantInt>(call->getOperand(0))->getZExtValue();

          // NOTE: Mesh shader input lowering is supposed to happen at the beginning of API mesh shader.
          m_builder->SetInsertPoint(&*beginMeshShaderBlock->getFirstInsertionPt());

          auto meshInput = getMeshInput(static_cast<BuiltInKind>(builtIn));
          assert(meshInput->getType() == call->getType());
          call->replaceAllUsesWith(meshInput);
        } else if (func.getName().startswith(lgcName::MeshTaskReadTaskPayload)) {
          // Read task payload
          assert(call->arg_size() == 1);

          auto byteOffset = call->getOperand(0);
          auto readValue = readTaskPayload(call->getType(), byteOffset);
          call->replaceAllUsesWith(readValue);
        } else if (func.getName().startswith(lgcName::MeshTaskWriteVertexOutput)) {
          // Write vertex output
          assert(call->arg_size() == 3);
          auto outputOffset = call->getOperand(0);
          auto vertexIndex = call->getOperand(1);
          auto outputValue = call->getOperand(2);

          writeVertexOutput(outputOffset, vertexIndex, outputValue);
        } else if (func.getName().startswith(lgcName::MeshTaskWritePrimitiveOutput)) {
          // Write primitive output
          assert(call->arg_size() == 3);
          auto outputOffset = call->getOperand(0);
          auto primitiveIndex = call->getOperand(1);
          auto outputValue = call->getOperand(2);

          writePrimitiveOutput(outputOffset, primitiveIndex, outputValue);
        } else {
          llvm_unreachable("Unknown mesh shader call!");
        }

        removedCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Set the actual output size of the primitives and vertices that the mesh shader workgroup will emit.
//
// @param vertexCount : Actual output size of the vertices
// @param primitiveCount : Actual output size of the primitives
void MeshTaskShader::setMeshOutputs(Value *vertexCount, Value *primitiveCount) {
  auto setMeshOutputsCall = m_builder->GetInsertPoint();

  auto checkSetMeshOutputsBlock = m_builder->GetInsertBlock();
  auto setMeshOutputsBlock = checkSetMeshOutputsBlock->splitBasicBlock(setMeshOutputsCall, ".setMeshOutputs");
  auto endSetMeshOutputsBlock = setMeshOutputsBlock->splitBasicBlock(setMeshOutputsCall, ".endSetMeshOutputs");

  // Modify ".checkSetMeshOutputs" block
  {
    m_builder->SetInsertPoint(checkSetMeshOutputsBlock->getTerminator());

    auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstThreadInSubgroup, setMeshOutputsBlock, endSetMeshOutputsBlock);
    checkSetMeshOutputsBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".setMeshOutputs" block
  {
    m_builder->SetInsertPoint(setMeshOutputsBlock->getTerminator());

    // Promote vertex/primitive count to SGPRs
    vertexCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertexCount);
    primitiveCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primitiveCount);

    // Check if vertex count or primitive count is zero. If so, set both to zero in order to disable vertex/primitive
    // exporting.
    auto zeroVertexCount = m_builder->CreateICmpEQ(vertexCount, m_builder->getInt32(0));
    auto zeroPrimitiveCount = m_builder->CreateICmpEQ(primitiveCount, m_builder->getInt32(0));
    auto hasZeroCount = m_builder->CreateOr(zeroVertexCount, zeroPrimitiveCount);
    vertexCount = m_builder->CreateSelect(hasZeroCount, m_builder->getInt32(0), vertexCount);
    primitiveCount = m_builder->CreateSelect(hasZeroCount, m_builder->getInt32(0), primitiveCount);

    // NOTE: Here, we promote vertex/primitive count to SGPRs once again because M0 implicitly used in s_sendmsg is
    // SGPR. LLVM backend has issues of handling this because it doesn't use s_cselect to translate LLVM IR select
    // instruction (which keeps the destination operand still in SGPR) and it doesn't use readfirstlane to promote
    // VGPR to SGPR for M0.
    vertexCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertexCount);
    primitiveCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primitiveCount);

    // M0[10:0] = vertexCount, M0[22:12] = primitiveCount
    Value *m0 = m_builder->CreateShl(primitiveCount, 12);
    m0 = m_builder->CreateOr(m0, vertexCount);
    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder->getInt32(GsAllocReq), m0});

    Value *ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    writeValueToLds(vertexCount, ldsOffset);

    ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveCount));
    writeValueToLds(primitiveCount, ldsOffset);
  }

  // Construct ".endSetMeshOutputs" block
  {
    m_builder->SetInsertPoint(endSetMeshOutputsBlock->getTerminator());

    // Currently, nothing to do
  }
}

// =====================================================================================================================
// Set primitive indices by forming primitive connectivity data and writing it to LDS.
//
// @param primitiveIndex : Primitive indexing
// @param primitiveIndices : All vertex index values that are used to form this primitive
void MeshTaskShader::setPrimitiveIndices(Value *primitiveIndex, Value *primitiveIndices) {
  //
  // HW requires the primitive connectivity data has the following bit layout:
  //   [31]    = Null primitive flag
  //   [28:20] = Index of vertex2
  //   [18:10] = Index of vertex1
  //   [8:0]   = Index of vertex0
  //
  auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  Value *primitiveData = nullptr;

  if (meshMode.outputPrimitive == OutputPrimitives::Points) {
    assert(primitiveIndices->getType() == m_builder->getInt32Ty()); // i32
    primitiveData = primitiveIndices;
  } else if (meshMode.outputPrimitive == OutputPrimitives::Lines) {
    assert(primitiveIndices->getType() == FixedVectorType::get(m_builder->getInt32Ty(), 2)); // v2i32
    Value *vertex0 = m_builder->CreateExtractElement(primitiveIndices, static_cast<uint64_t>(0));
    Value *vertex1 = m_builder->CreateExtractElement(primitiveIndices, 1);

    primitiveData = m_builder->CreateShl(vertex1, 10);
    primitiveData = m_builder->CreateOr(primitiveData, vertex0);
  } else {
    assert(meshMode.outputPrimitive == OutputPrimitives::Triangles);
    Value *vertex0 = m_builder->CreateExtractElement(primitiveIndices, static_cast<uint64_t>(0));
    Value *vertex1 = m_builder->CreateExtractElement(primitiveIndices, 1);
    Value *vertex2 = m_builder->CreateExtractElement(primitiveIndices, 2);

    primitiveData = m_builder->CreateShl(vertex2, 10);
    primitiveData = m_builder->CreateOr(primitiveData, vertex1);
    primitiveData = m_builder->CreateShl(primitiveData, 10);
    primitiveData = m_builder->CreateOr(primitiveData, vertex0);
  }

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder->CreateAdd(ldsStart, primitiveIndex);

  // NOTE: We first clear old primitive connectivity data and use atomic OR operation to set new data. This is because
  // the null primitive flag might be set via built-in CullPrimitive.
  static const unsigned ClearMask = (1u << 31);
  atomicOpWithLds(AtomicRMWInst::And, m_builder->getInt32(ClearMask), ldsOffset);
  atomicOpWithLds(AtomicRMWInst::Or, primitiveData, ldsOffset);
}

// =====================================================================================================================
// Set primitive culled state by writing the null primitive flag to LDS.
//
// @param primitiveIndex : Primitive indexing
// @param isCulled : Whether this primitive is culled
void MeshTaskShader::setPrimitiveCulled(Value *primitiveIndex, Value *isCulled) {
  //
  // HW requires the primitive connectivity data has the following bit layout:
  //   [31]    = Null primitive flag
  //   [28:20] = Index of vertex2
  //   [18:10] = Index of vertex1
  //   [8:0]   = Index of vertex0
  //
  assert(isCulled->getType()->isIntegerTy(1));

  static const unsigned NullPrimitive = (1u << 31);
  auto nullPrimitive = m_builder->CreateSelect(isCulled, m_builder->getInt32(NullPrimitive), m_builder->getInt32(0));

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder->CreateAdd(ldsStart, primitiveIndex);

  // NOTE: We first clear null primitive flag and use atomic OR operation to set new flag. This is because the
  // primitive connectivity data might be set via built-in PrimitiveXXXIndices.
  static const unsigned ClearMask = ~(1u << 31);
  atomicOpWithLds(AtomicRMWInst::And, m_builder->getInt32(ClearMask), ldsOffset);
  atomicOpWithLds(AtomicRMWInst::Or, nullPrimitive, ldsOffset);
}

// =====================================================================================================================
// Get mesh built-in input.
//
// @param builtIn : Input built-in ID of mesh shader
// @returns : Value of the specified input built-in
Value *MeshTaskShader::getMeshInput(BuiltInKind builtIn) {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  switch (builtIn) {
  case BuiltInDrawIndex: {
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
    return getFunctionArgument(entryPoint, entryArgIdxs.drawIndex);
  }

  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().enableMultiView) {
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
      return getFunctionArgument(entryPoint, entryArgIdxs.viewIndex);
    }
    return m_builder->getInt32(0);
  }

  case BuiltInNumWorkgroups:
    return getMeshNumWorkgroups();

  case BuiltInWorkgroupId:
    return getMeshWorkgroupId();

  case BuiltInLocalInvocationId:
    return getMeshLocalInvocationId();

  case BuiltInGlobalInvocationId:
    return getMeshGlobalInvocationId();

  case BuiltInLocalInvocationIndex:
    return getMeshLocalInvocationIndex();

  case BuiltInSubgroupId: {
    // subgroupId = localInvocationIndex / subgroupSize
    auto localInvocationIndex = getMeshLocalInvocationIndex();
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStageMesh);
    assert(subgroupSize > 0 && subgroupSize % 32 == 0);
    return m_builder->CreateLShr(localInvocationIndex, m_builder->getInt32(Log2_32(subgroupSize)));
  }

  case BuiltInNumSubgroups: {
    // numSubgroups = numMeshThreads / subgroupSize
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStageMesh);
    assert(subgroupSize > 0 && subgroupSize % 32 == 0);
    const unsigned numSubgroups = alignTo(numMeshThreads, subgroupSize) / subgroupSize;
    return m_builder->getInt32(numSubgroups);
  }

  default:
    llvm_unreachable("Unknown mesh input built-in!");
    return nullptr;
  }
}

// =====================================================================================================================
// Write mesh shader vertex outputs to LDS.
//
// @param outputOffset : Relative offset of this output (in dwords) within all outputs of the indexed vertex
// @param vertexIndex : Vertex indexing
// @param outputValue : Output value to write
void MeshTaskShader::writeVertexOutput(Value *outputOffset, Value *vertexIndex, Value *outputValue) {
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
  const unsigned vertexStride = 4 * resUsage->inOutUsage.outputMapLocCount; // Corresponds to vec4 output

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexOutput));
  Value *ldsOffset = m_builder->CreateMul(vertexIndex, m_builder->getInt32(vertexStride));
  ldsOffset = m_builder->CreateAdd(ldsOffset, outputOffset);
  ldsOffset = m_builder->CreateAdd(ldsStart, ldsOffset);

  writeValueToLds(outputValue, ldsOffset);
}

// =====================================================================================================================
// Write mesh shader primitive outputs to LDS.
//
// @param outputOffset : Relative offset of this output (in dwords) within all outputs of the indexed primitive
// @param vertexIndex : Primitive indexing
// @param outputValue : Output value to write
void MeshTaskShader::writePrimitiveOutput(Value *outputOffset, Value *primitiveIndex, Value *outputValue) {
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
  const unsigned primitiveStride = 4 * resUsage->inOutUsage.perPrimitiveOutputMapLocCount; // Corresponds to vec4 output

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveOutput));
  Value *ldsOffset = m_builder->CreateMul(primitiveIndex, m_builder->getInt32(primitiveStride));
  ldsOffset = m_builder->CreateAdd(ldsOffset, outputOffset);
  ldsOffset = m_builder->CreateAdd(ldsStart, ldsOffset);

  writeValueToLds(outputValue, ldsOffset);
}

// =====================================================================================================================
// Export primitive (primitive connectivity data, primitive payload, and primitive attributes).
void MeshTaskShader::exportPrimitive() {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage;

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder->CreateAdd(ldsStart, m_waveThreadInfo.primOrVertexIndex);

  // The first dword is primitive connectivity data
  auto primitiveIndices = readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

  // The second dword is primitive payload, which has the following bit layout specified by HW:
  //   [31:30] = VRS rate Y
  //   [29:28] = VRS rate X
  //   [27:24] = Unused
  //   [23:20] = Viewport index
  //   [19:17] = Render target slice index
  //   [16:0]  = Pipeline primitive ID
  Value *primitivePayload = nullptr;
  Value *primitiveId = nullptr;
  if (builtInUsage.primitiveId) {
    // [16:0] = Pipeline primitive ID
    primitiveId = readMeshBuiltInFromLds(BuiltInPrimitiveId);
    auto primitiveIdMaskAndShift = m_builder->CreateAnd(primitiveId, 0x1FFFF);
    if (primitivePayload)
      primitivePayload = m_builder->CreateOr(primitivePayload, primitiveIdMaskAndShift);
    else
      primitivePayload = primitiveIdMaskAndShift;
  }

  Value *layer = nullptr;
  if (builtInUsage.layer)
    layer = readMeshBuiltInFromLds(BuiltInLayer);

  Value *viewIndex = nullptr;
  const bool enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
  if (enableMultiView) {
    auto entryPoint = m_builder->GetInsertBlock()->getParent();
    const auto entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
    viewIndex = getFunctionArgument(entryPoint, entryArgIdxs.viewIndex);
  }

  if (enableMultiView || builtInUsage.layer) {
    // [19:17] = Render target slice index
    // When multi-view is enabled, the input view index is treated as the output layer.
    auto layerMaskAndShift = m_builder->CreateAnd(enableMultiView ? viewIndex : layer, 0x7);
    layerMaskAndShift = m_builder->CreateShl(layerMaskAndShift, 17);
    if (primitivePayload)
      primitivePayload = m_builder->CreateOr(primitivePayload, layerMaskAndShift);
    else
      primitivePayload = layerMaskAndShift;
  }

  Value *viewportIndex = nullptr;
  if (builtInUsage.viewportIndex) {
    // [23:20] = Viewport index
    viewportIndex = readMeshBuiltInFromLds(BuiltInViewportIndex);
    auto viewportIndexMaskAndShift = m_builder->CreateAnd(viewportIndex, 0xF);
    viewportIndexMaskAndShift = m_builder->CreateShl(viewportIndexMaskAndShift, 20);
    if (primitivePayload)
      primitivePayload = m_builder->CreateOr(primitivePayload, viewportIndexMaskAndShift);
    else
      primitivePayload = viewportIndexMaskAndShift;
  }

  if (builtInUsage.primitiveShadingRate) {
    // [31:28] = VRS rate
    auto primitiveShadingRate = readMeshBuiltInFromLds(BuiltInPrimitiveShadingRate);
    auto hwShadingRateMaskAndShift = convertToHwShadingRate(primitiveShadingRate);

    hwShadingRateMaskAndShift = m_builder->CreateAnd(hwShadingRateMaskAndShift, 0xF);
    hwShadingRateMaskAndShift = m_builder->CreateShl(hwShadingRateMaskAndShift, 28);

    if (primitivePayload)
      primitivePayload = m_builder->CreateOr(primitivePayload, hwShadingRateMaskAndShift);
    else
      primitivePayload = hwShadingRateMaskAndShift;
  }

  doExport(ExportKind::Prim, ExportInfo{0, {primitiveIndices, primitivePayload}});

  // Primitive attribute export follows vertex attribute export
  SmallVector<ExportInfo, 32> primAttrExports;

  unsigned startLoc = inOutUsage.mesh.genericOutputMapLocCount;
  for (auto &builtInExport : inOutUsage.mesh.builtInExportLocs) {
    const unsigned exportLoc = builtInExport.second;
    startLoc = std::max(startLoc, exportLoc + 1);
  }

  // Export primitive attributes (from generic outputs)
  ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveOutput));
  auto primitiveStride = 4 * inOutUsage.perPrimitiveOutputMapLocCount;
  auto ldsOffsetBase = m_builder->CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder->getInt32(primitiveStride));
  ldsOffsetBase = m_builder->CreateAdd(ldsStart, ldsOffsetBase);

  for (unsigned loc = 0; loc < inOutUsage.mesh.perPrimitiveGenericOutputMapLocCount; ++loc) {
    auto ldsOffset = m_builder->CreateAdd(ldsOffsetBase, m_builder->getInt32(4 * loc));
    auto exportValue = readValueFromLds(FixedVectorType::get(m_builder->getFloatTy(), 4), ldsOffset);

    std::array<Value *, 4> exportValues;
    for (unsigned j = 0; j < 4; ++j)
      exportValues[j] = m_builder->CreateExtractElement(exportValue, j);

    primAttrExports.push_back({startLoc + loc, exportValues});
    ++inOutUsage.primExpCount;
  }

  // Export primitive attributes (from built-ins as generic ones)
  if (builtInUsage.primitiveId) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInPrimitiveId) > 0) {
      assert(primitiveId);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInPrimitiveId];
      primAttrExports.push_back({startLoc + exportLoc, primitiveId});
      ++inOutUsage.primExpCount;
    }
  }

  bool exportLayer = false;
  if (builtInUsage.layer) {
    exportLayer = true;
  } else {
    const auto nextStage = m_pipelineState->getNextShaderStage(ShaderStageMesh);
    if (nextStage == ShaderStageFragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      if (fsBuiltInUsage.layer) {
        // NOTE: In such case, mesh shader doesn't export layer while fragment shader expects to read it. We
        // export 0 to fragment shader, which is required by the spec.
        layer = m_builder->getInt32(0);
        exportLayer = true;
      }
    }
  }

  if (exportLayer) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInLayer) > 0) {
      assert(layer);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInLayer];
      primAttrExports.push_back({startLoc + exportLoc, layer});
      ++inOutUsage.primExpCount;
    }
  }

  if (enableMultiView) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInViewIndex) > 0) {
      assert(viewIndex);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInViewIndex];
      primAttrExports.push_back({startLoc + exportLoc, viewIndex});
      ++inOutUsage.primExpCount;
    }
  }

  bool exportViewportIndex = false;
  if (builtInUsage.viewportIndex) {
    exportViewportIndex = true;
  } else {
    const auto nextStage = m_pipelineState->getNextShaderStage(ShaderStageMesh);
    if (nextStage == ShaderStageFragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      if (fsBuiltInUsage.viewportIndex) {
        // NOTE: In such case, mesh shader doesn't export viewport index while fragment shader expects to read it. We
        // export 0 to fragment shader, which is required by spec.
        viewportIndex = m_builder->getInt32(0);
        exportViewportIndex = true;
      }
    }
  }

  if (exportViewportIndex) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInViewportIndex) > 0) {
      assert(viewportIndex);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInViewportIndex];
      primAttrExports.push_back({startLoc + exportLoc, viewportIndex});
      ++inOutUsage.primExpCount;
    }
  }

  doExport(ExportKind::PrimAttr, primAttrExports);
}

// =====================================================================================================================
// Export vertex (vertex position data and vertex attributes).
void MeshTaskShader::exportVertex() {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage;

  // Export vertex position data
  SmallVector<ExportInfo, 8> posExports;

  if (builtInUsage.position) {
    auto position = readMeshBuiltInFromLds(BuiltInPosition);
    std::array<Value *, 4> positions = {m_builder->CreateExtractElement(position, static_cast<uint64_t>(0)),
                                        m_builder->CreateExtractElement(position, 1),
                                        m_builder->CreateExtractElement(position, 2),
                                        m_builder->CreateExtractElement(position, 3)};

    posExports.push_back({0, positions});
  }

  if (builtInUsage.pointSize) {
    auto pointSize = readMeshBuiltInFromLds(BuiltInPointSize);
    posExports.push_back({1, pointSize});
  }

  SmallVector<Value *, 8> clipDistances;
  if (builtInUsage.clipDistance > 0) {
    auto clipDistance = readMeshBuiltInFromLds(BuiltInClipDistance);
    for (unsigned i = 0; i < builtInUsage.clipDistance; ++i)
      clipDistances.push_back(m_builder->CreateExtractElement(clipDistance, i));
  }

  SmallVector<Value *, 8> cullDistances;
  if (builtInUsage.cullDistance > 0) {
    auto cullDistance = readMeshBuiltInFromLds(BuiltInCullDistance);
    for (unsigned i = 0; i < builtInUsage.cullDistance; ++i)
      cullDistances.push_back(m_builder->CreateExtractElement(cullDistance, i));
  }

  SmallVector<Value *, 8> clipCullDistances;
  if (builtInUsage.clipDistance > 0 || builtInUsage.cullDistance > 0) {
    assert(builtInUsage.clipDistance + builtInUsage.cullDistance <= MaxClipCullDistanceCount);

    // Merge clipDistance and cullDistance
    for (auto clipDistance : clipDistances)
      clipCullDistances.push_back(clipDistance);

    for (auto cullDistance : cullDistances)
      clipCullDistances.push_back(cullDistance);

    // Do array padding
    auto undef = PoisonValue::get(m_builder->getFloatTy());
    if (clipCullDistances.size() <= 4) {
      while (clipCullDistances.size() < 4) // <4 x float>
        clipCullDistances.push_back(undef);
    } else {
      while (clipCullDistances.size() < 8) // <8 x float>
        clipCullDistances.push_back(undef);
    }

    unsigned pos = builtInUsage.pointSize ? 2 : 1;
    posExports.push_back(
        {pos, {clipCullDistances[0], clipCullDistances[1], clipCullDistances[2], clipCullDistances[3]}});

    if (clipCullDistances.size() > 4) {
      // Do the second exporting
      posExports.push_back(
          {pos + 1, {clipCullDistances[4], clipCullDistances[5], clipCullDistances[6], clipCullDistances[7]}});
    }
  }

  doExport(ExportKind::Pos, posExports);

  SmallVector<ExportInfo, 32> vertAttrExports;

  // Export vertex attributes (from generic outputs)
  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexOutput));
  auto vertexStride = 4 * inOutUsage.outputMapLocCount;
  auto ldsOffsetBase = m_builder->CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder->getInt32(vertexStride));
  ldsOffsetBase = m_builder->CreateAdd(ldsStart, ldsOffsetBase);

  for (unsigned i = 0; i < inOutUsage.mesh.genericOutputMapLocCount; ++i) {
    auto ldsOffset = m_builder->CreateAdd(ldsOffsetBase, m_builder->getInt32(4 * i));
    auto exportValue = readValueFromLds(FixedVectorType::get(m_builder->getFloatTy(), 4), ldsOffset);

    std::array<Value *, 4> exportValues = {m_builder->CreateExtractElement(exportValue, static_cast<uint64_t>(0)),
                                           m_builder->CreateExtractElement(exportValue, 1),
                                           m_builder->CreateExtractElement(exportValue, 2),
                                           m_builder->CreateExtractElement(exportValue, 3)};

    vertAttrExports.push_back({i, exportValues});
    ++inOutUsage.expCount;
  }

  // Export vertex attributes (from built-ins as generic ones)
  if (builtInUsage.clipDistance > 0 || builtInUsage.cullDistance > 0) {
    bool exportClipCullDistance = true;

    auto nextStage = m_pipelineState->getNextShaderStage(ShaderStageMesh);
    if (nextStage == ShaderStageFragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

      exportClipCullDistance = fsBuiltInUsage.clipDistance > 0 || fsBuiltInUsage.cullDistance > 0;
      if (exportClipCullDistance) {
        // NOTE: We adjust the size of clipDistance and cullDistance according to their usages in fragment shader.
        const unsigned clipDistanceCount = std::min(fsBuiltInUsage.clipDistance, builtInUsage.clipDistance);
        const unsigned cullDistanceCount = std::min(fsBuiltInUsage.cullDistance, builtInUsage.cullDistance);

        auto undef = PoisonValue::get(m_builder->getFloatTy());

        clipCullDistances.clear();
        for (unsigned i = 0; i < clipDistanceCount; ++i)
          clipCullDistances.push_back(clipDistances[i]);

        for (unsigned i = clipDistanceCount; i < fsBuiltInUsage.clipDistance; ++i)
          clipCullDistances.push_back(undef);

        for (unsigned i = 0; i < cullDistanceCount; ++i)
          clipCullDistances.push_back(cullDistances[i]);

        // Do array padding
        if (clipCullDistances.size() <= 4) {
          while (clipCullDistances.size() < 4) // <4 x float>
            clipCullDistances.push_back(undef);
        } else {
          while (clipCullDistances.size() < 8) // <8 x float>
            clipCullDistances.push_back(undef);
        }
      }
    }

    if (exportClipCullDistance) {
      unsigned exportLoc = InvalidValue;
      if (inOutUsage.mesh.builtInExportLocs.count(BuiltInClipDistance) > 0) {
        exportLoc = inOutUsage.mesh.builtInExportLocs[BuiltInClipDistance];
      } else {
        assert(inOutUsage.mesh.builtInExportLocs.count(BuiltInCullDistance) > 0);
        exportLoc = inOutUsage.mesh.builtInExportLocs[BuiltInCullDistance];
      }
      assert(exportLoc != InvalidValue);

      vertAttrExports.push_back(
          {exportLoc, {clipCullDistances[0], clipCullDistances[1], clipCullDistances[2], clipCullDistances[3]}});
      ++inOutUsage.expCount;

      if (clipCullDistances.size() > 4) {
        // Do the second exporting
        vertAttrExports.push_back(
            {exportLoc + 1, {clipCullDistances[4], clipCullDistances[5], clipCullDistances[6], clipCullDistances[7]}});
        ++inOutUsage.expCount;
      }
    }
  }

  doExport(ExportKind::VertAttr, vertAttrExports);
}

// =====================================================================================================================
// Collect mesh shader statistics and write this info to mesh pipeline statistics buffer.
//
// @param entryPoint : Entry-point of mesh shader
// @param numMeshPrimitives : Actual number of primitives emitted by mesh shader
void MeshTaskShader::collectMeshStatsInfo(Function *entryPoint, Value *numMeshPrimitives) {
  if (!m_pipelineState->needSwMeshPipelineStats())
    return;

  auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const uint64_t numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;

  Value *meshPipeStatsBufPtr = m_pipelineSysValues.get(entryPoint)->getMeshPipeStatsBufPtr();

  //
  // Record numMeshThreads
  //
  {
    Value *meshPipeStatsBufEntryPtr = m_builder->CreateGEP(
        m_builder->getInt8Ty(), meshPipeStatsBufPtr, m_builder->getInt32(offsetof(MeshPipeStatsEntry, numMeshThreads)));
    meshPipeStatsBufEntryPtr = m_builder->CreateBitCast(meshPipeStatsBufEntryPtr,
                                                        PointerType::get(m_builder->getInt64Ty(), ADDR_SPACE_GLOBAL));

    // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
    // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
    // move the added value to VGPR to mark it as "divergent".
    Value *valueToAdd = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
    valueToAdd = m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numMeshThreads)),
                                                static_cast<uint64_t>(0));
    valueToAdd =
        m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numMeshThreads >> 32)), 1);
    valueToAdd = m_builder->CreateBitCast(valueToAdd, m_builder->getInt64Ty());

    m_builder->CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                               AtomicOrdering::Monotonic, SyncScope::System);
  }

  //
  // Record numMeshPrimitives
  //
  {
    Value *meshPipeStatsBufEntryPtr =
        m_builder->CreateGEP(m_builder->getInt8Ty(), meshPipeStatsBufPtr,
                             m_builder->getInt32(offsetof(MeshPipeStatsEntry, numMeshPrimitives)));
    meshPipeStatsBufEntryPtr = m_builder->CreateBitCast(meshPipeStatsBufEntryPtr,
                                                        PointerType::get(m_builder->getInt64Ty(), ADDR_SPACE_GLOBAL));

    assert(numMeshPrimitives->getType() == m_builder->getInt32Ty());

    // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
    // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
    // move the added value to VGPR to mark it as "divergent".
    Value *valueToAdd = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
    valueToAdd =
        m_builder->CreateInsertElement(valueToAdd, convertToDivergent(numMeshPrimitives), static_cast<uint64_t>(0));
    valueToAdd = m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(0)), 1);
    valueToAdd = m_builder->CreateBitCast(valueToAdd, m_builder->getInt64Ty());

    m_builder->CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                               AtomicOrdering::Monotonic, SyncScope::System);
  }
}

// =====================================================================================================================
// Do exporting. The array of values for certain export kind are all exported.
//
// @param kind : Export kind (positions, primitive, or parameters)
// @param exports : Array of exports
void MeshTaskShader::doExport(ExportKind kind, ArrayRef<ExportInfo> exports) {
  for (unsigned i = 0; i < exports.size(); ++i) {
    auto &values = exports[i].values;
    assert(values.size() == 4); // Must be at most 4 export values

    assert(values[0]); // Must at least have one value
    auto valueTy = values[0]->getType();
    assert(valueTy->isFloatTy() || valueTy->isIntegerTy(32)); // Must be float or i32

    auto undef = PoisonValue::get(valueTy);
    unsigned validMask = 0;
    for (unsigned j = 0; j < 4; ++j) {
      if (values[j])
        validMask |= (1U << j);
    }

    unsigned target = InvalidValue;
    switch (kind) {
    case ExportKind::Pos:
      target = EXP_TARGET_POS_0;
      break;
    case ExportKind::Prim:
      target = EXP_TARGET_PRIM;
      break;
    case ExportKind::VertAttr:
    case ExportKind::PrimAttr:
      target = EXP_TARGET_PARAM_0;
      break;
    default:
      llvm_unreachable("Unexpected export target!");
      break;
    }

    bool exportDone = false;
    if ((kind == ExportKind::Pos || kind == ExportKind::Prim) && i == exports.size() - 1)
      exportDone = true; // Last export

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, valueTy,
                               {
                                   m_builder->getInt32(target + exports[i].index), // tgt
                                   m_builder->getInt32(validMask),                 // en
                                   values[0],                                      // src0
                                   values[1] ? values[1] : undef,                  // src1
                                   values[2] ? values[2] : undef,                  // src2
                                   values[3] ? values[3] : undef,                  // src3
                                   m_builder->getInt1(exportDone),                 // done
                                   m_builder->getFalse(),                          // vm
                               });
  }
}

// =====================================================================================================================
// Get the flat workgroup ID of mesh shader.
//
// @returns : Value of flat workgroup ID
Value *MeshTaskShader::getMeshFlatWorkgroupId() {
  assert(getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  if (!m_meshFlatWorkgroupId) {
    auto ldsOffset = m_builder->getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::FlatWorkgroupId));
    auto flatWorkgroupId = readValueFromLds(m_builder->getInt32Ty(), ldsOffset);
    flatWorkgroupId =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, flatWorkgroupId); // Promoted to SGPR
    flatWorkgroupId->setName("flatWorkgroupId");

    m_meshFlatWorkgroupId = flatWorkgroupId;
  }

  return m_meshFlatWorkgroupId;
}

// =====================================================================================================================
// Get the built-in numWorkgroups of mesh shader.
//
// @returns : Value of the built-in numWorkgroups
Value *MeshTaskShader::getMeshNumWorkgroups() {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
  return getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);
}

// =====================================================================================================================
// Get the built-in WorkgroupId of mesh shader.
//
// @returns : Value of the built-in WorkgroupId
Value *MeshTaskShader::getMeshWorkgroupId() {
  auto entryPoint = m_builder->GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  if (!m_meshWorkgroupId) {
    // flatWorkgroupId = workgroupId.z * dispatchDims.x * dispatchDims.y +
    //                   workgroupId.y * dispatchDims.x + workgroupId.x
    //
    // workgroupId.z = flatWorkgroupId / dispatchDims.x * dispatchDims.y
    // workgroupId.y = (flatWorkgroupId - dispatchDims.x * dispatchDims.y * workgroupId.z) / dispatchDims.x
    // workgroupId.x = (flatWorkgroupId - dispatchDims.x * dispatchDims.y * workgroupId.z) -
    //                 dispatchDims.x * workgroupId.y
    auto flatWorkgroupId = getMeshFlatWorkgroupId();

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;

    auto dispatchDims = getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);
    auto dispatchDimX = m_builder->CreateExtractElement(dispatchDims, static_cast<uint64_t>(0));
    auto dispatchDimY = m_builder->CreateExtractElement(dispatchDims, 1);
    auto dispatchDimXMulY = m_builder->CreateMul(dispatchDimX, dispatchDimY);

    auto workgroupIdZ = m_builder->CreateUDiv(flatWorkgroupId, dispatchDimXMulY);
    workgroupIdZ = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, workgroupIdZ); // Promoted to SGPR

    auto diff = m_builder->CreateMul(dispatchDimXMulY, workgroupIdZ);
    diff = m_builder->CreateSub(flatWorkgroupId, diff);
    auto workgroupIdY = m_builder->CreateUDiv(diff, dispatchDimX);
    workgroupIdY = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, workgroupIdY); // Promoted to SGPR

    auto workgroupIdX = m_builder->CreateMul(dispatchDimX, workgroupIdY);
    workgroupIdX = m_builder->CreateSub(diff, workgroupIdX);
    workgroupIdX = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, workgroupIdX); // Promoted to SGPR

    Value *workgroupId = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 3));
    workgroupId = m_builder->CreateInsertElement(workgroupId, workgroupIdX, static_cast<uint64_t>(0));
    workgroupId = m_builder->CreateInsertElement(workgroupId, workgroupIdY, 1);
    workgroupId = m_builder->CreateInsertElement(workgroupId, workgroupIdZ, 2);

    m_meshWorkgroupId = workgroupId;
    m_meshWorkgroupId->setName("workgroupId");
  }

  return m_meshWorkgroupId;
}

// =====================================================================================================================
// Get the built-in LocalInvocationId of mesh shader.
//
// @returns : Value of the built-in LocalInvocationId
Value *MeshTaskShader::getMeshLocalInvocationId() {
  assert(getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  if (!m_meshLocalInvocationId) {
    // localInvocationIndex = localInvocationId.z * workgroupSize.x * workgroupSize.y +
    //                        localInvocationId.y * workgroupSize.x + localInvocationId.x
    //
    // localInvocationId.z = localInvocationIndex / workgroupSize.x * workgroupSize.y
    // localInvocationId.y = (localInvocationIndex - workgroupSize.x * workgroupSize.y * localInvocationId.z) /
    //                       workgroupSize.x
    // localInvocationId.x = (localInvocationIndex - workgroupSize.x * workgroupSize.y * localInvocationId.z) -
    //                       workgroupSize.x * localInvocationId.y
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    auto localInvocationIndex = getMeshLocalInvocationIndex();

    auto workgroupSizeX = m_builder->getInt32(meshMode.workgroupSizeX);
    auto workgroupSizeXMulY = m_builder->getInt32(meshMode.workgroupSizeX * meshMode.workgroupSizeY);

    auto localInvocationIdZ = m_builder->CreateUDiv(localInvocationIndex, workgroupSizeXMulY);

    auto diff = m_builder->CreateMul(workgroupSizeXMulY, localInvocationIdZ);
    diff = m_builder->CreateSub(localInvocationIndex, diff);
    auto localInvocationIdY = m_builder->CreateUDiv(diff, workgroupSizeX);

    auto localInvocationIdX = m_builder->CreateMul(workgroupSizeX, localInvocationIdY);
    localInvocationIdX = m_builder->CreateSub(diff, localInvocationIdX);

    Value *localInvocationId = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 3));
    localInvocationId = m_builder->CreateInsertElement(localInvocationId, localInvocationIdX, static_cast<uint64_t>(0));
    localInvocationId = m_builder->CreateInsertElement(localInvocationId, localInvocationIdY, 1);
    localInvocationId = m_builder->CreateInsertElement(localInvocationId, localInvocationIdZ, 2);

    m_meshLocalInvocationId = localInvocationId;
    m_meshLocalInvocationId->setName("localInvocationId");
  }

  return m_meshLocalInvocationId;
}

// =====================================================================================================================
// Get the built-in LocalInvocationIndex of mesh shader.
//
// @returns : Value of the built-in LocalInvocationIndex
Value *MeshTaskShader::getMeshLocalInvocationIndex() {
  assert(getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader
  return m_waveThreadInfo.threadIdInSubgroup;
}

// =====================================================================================================================
// Get the built-in GlobalInvocationId of mesh shader.
//
// @returns : Value of the built-in GlobalInvocationId
Value *MeshTaskShader::getMeshGlobalInvocationId() {
  assert(getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  if (!m_meshGlobalInvocationId) {
    // globalInvocationId = workgroupId * workgroupSize + localInvocationId
    auto workgourpId = getMeshWorkgroupId();
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    auto workgroupSize =
        ConstantVector::get({m_builder->getInt32(meshMode.workgroupSizeX), m_builder->getInt32(meshMode.workgroupSizeY),
                             m_builder->getInt32(meshMode.workgroupSizeZ)});
    auto localInvocationId = getMeshLocalInvocationId();

    auto globalInvocationId = m_builder->CreateMul(workgourpId, workgroupSize);
    globalInvocationId = m_builder->CreateAdd(globalInvocationId, localInvocationId);

    m_meshGlobalInvocationId = globalInvocationId;
    m_meshGlobalInvocationId->setName("globalInvocationId");
  }

  return m_meshGlobalInvocationId;
}

// =====================================================================================================================
// Get the global invocation index (equivalent to flat built-in GlobalInvocationId) of mesh shader.
//
// @returns : Value of global invocation index
Value *MeshTaskShader::getGlobalInvocationIndex() {
  assert(getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  if (!m_meshGlobalInvocationIndex) {
    // globalInvocationIndex = flatWorkgroupId * numMeshThreads + threadIdInSubgroup
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
    auto flatWorkgroupId = getMeshFlatWorkgroupId();

    Value *localInvocationIndex = getMeshLocalInvocationIndex();
    Value *globalInvocationIndex = m_builder->CreateMul(flatWorkgroupId, m_builder->getInt32(numMeshThreads));
    globalInvocationIndex = m_builder->CreateAdd(globalInvocationIndex, localInvocationIndex);

    m_meshGlobalInvocationIndex = globalInvocationIndex;
    m_meshGlobalInvocationIndex->setName("globalInvocationIndex");
  }

  return m_meshGlobalInvocationIndex;
}

// =====================================================================================================================
// Read mesh shader built-in value from LDS, which is supposed to be written by mesh shader execution.
//
// @param builtIn : Mesh shader built-in
// @returns : The built-in value from LDS
Value *MeshTaskShader::readMeshBuiltInFromLds(BuiltInKind builtIn) {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage;

  bool isPerPrimitive = (builtIn == BuiltInPrimitiveId || builtIn == BuiltInViewportIndex || builtIn == BuiltInLayer ||
                         builtIn == BuiltInPrimitiveShadingRate);

  unsigned location = InvalidValue;
  MeshLdsRegion region = MeshLdsRegion::VertexOutput;

  if (isPerPrimitive) {
    assert(inOutUsage.perPrimitiveBuiltInOutputLocMap.count(builtIn) > 0);
    location = inOutUsage.perPrimitiveBuiltInOutputLocMap[builtIn];
    region = MeshLdsRegion::PrimitiveOutput;
  } else {
    assert(inOutUsage.builtInOutputLocMap.count(builtIn) > 0);
    location = inOutUsage.builtInOutputLocMap[builtIn];
    region = MeshLdsRegion::VertexOutput;
  }

  Type *readTy = nullptr;
  switch (builtIn) {
  case BuiltInPosition:
    assert(builtInUsage.position);
    readTy = FixedVectorType::get(m_builder->getFloatTy(), 4);
    break;
  case BuiltInPointSize:
    assert(builtInUsage.pointSize);
    readTy = m_builder->getFloatTy();
    break;
  case BuiltInClipDistance:
    assert(builtInUsage.clipDistance > 0);
    readTy = FixedVectorType::get(m_builder->getFloatTy(), builtInUsage.clipDistance);
    break;
  case BuiltInCullDistance:
    assert(builtInUsage.cullDistance > 0);
    readTy = FixedVectorType::get(m_builder->getFloatTy(), builtInUsage.cullDistance);
    break;
  case BuiltInPrimitiveId:
    assert(builtInUsage.primitiveId);
    readTy = m_builder->getInt32Ty();
    break;
  case BuiltInViewportIndex:
    assert(builtInUsage.viewportIndex);
    readTy = m_builder->getInt32Ty();
    break;
  case BuiltInLayer:
    assert(builtInUsage.layer);
    readTy = m_builder->getInt32Ty();
    break;
  case BuiltInPrimitiveShadingRate:
    assert(builtInUsage.primitiveShadingRate);
    readTy = m_builder->getInt32Ty();
    break;
  default:
    llvm_unreachable("Unexpected mesh shader built-in!");
    break;
  }

  Value *ldsOffset = nullptr;
  if (region == MeshLdsRegion::VertexOutput) {
    auto vertexStride = 4 * inOutUsage.outputMapLocCount;
    ldsOffset = m_builder->CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder->getInt32(vertexStride));
  } else {
    assert(region == MeshLdsRegion::PrimitiveOutput);
    auto primitiveStride = 4 * inOutUsage.perPrimitiveOutputMapLocCount;
    ldsOffset = m_builder->CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder->getInt32(primitiveStride));
  }
  ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(4 * location));

  Value *ldsStart = m_builder->getInt32(getMeshShaderLdsRegionStart(region));
  ldsOffset = m_builder->CreateAdd(ldsStart, ldsOffset);

  return readValueFromLds(readTy, ldsOffset);
}

// =====================================================================================================================
// Change primitive shading rate from API to HW-specific shading rate.
//
// @param primitiveShadingRate : Primitive shading rate from API
// @returns : HW-specific shading rate
Value *MeshTaskShader::convertToHwShadingRate(Value *primitiveShadingRate) {
  assert(m_gfxIp == GfxIpVersion({10, 3})); // Must be GFX10.3

  // NOTE: The shading rates have different meanings in HW and LGC interface. GFX10.3 HW supports 2-pixel mode
  // and 4-pixel mode is not supported. But the spec requires us to accept unsupported rates and clamp them to
  // maxFragmentSize of HW. The mapping is therefore as follow:
  //
  //   VRS rate X: MaskNone -> 0b00, Horizontal2Pixels | Horizontal4Pixels -> 0b01
  //   VRS rate Y: MaskNone -> 0b00, Vertical2Pixels | Vertical4Pixels -> 0b01
  //
  // hwXRate = (primitiveShadingRate & (Horizontal2Pixels | Horizontal4Pixels)) ? 0x1 : 0x0
  Value *xRate2Pixels = m_builder->CreateAnd(
      primitiveShadingRate, m_builder->getInt32(ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels));
  xRate2Pixels = m_builder->CreateICmpNE(xRate2Pixels, m_builder->getInt32(0));
  Value *hwXRate = m_builder->CreateSelect(xRate2Pixels, m_builder->getInt32(1), m_builder->getInt32(0));

  // yRate = (primitiveShadingRate & (Vertical2Pixels | Vertical4Pixels)) ? 0x1 : 0x0
  Value *yRate2Pixels = m_builder->CreateAnd(
      primitiveShadingRate, m_builder->getInt32(ShadingRateVertical2Pixels | ShadingRateVertical4Pixels));
  yRate2Pixels = m_builder->CreateICmpNE(yRate2Pixels, m_builder->getInt32(0));
  Value *hwYRate = m_builder->CreateSelect(yRate2Pixels, m_builder->getInt32(1), m_builder->getInt32(0));

  // hwShadingRate = (hwYRate << 2) | hwXRate
  auto hwShadingRate = m_builder->CreateShl(hwYRate, 2);
  hwShadingRate = m_builder->CreateOr(hwShadingRate, hwXRate);

  return hwShadingRate;
}

// =====================================================================================================================
// Read value from mesh shader LDS.
//
// @param readTy : Type of value to read
// @param ldsOffset : LDS offset in dwords
// @returns : The Value read from LDS
Value *MeshTaskShader::readValueFromLds(Type *readTy, Value *ldsOffset) {
  assert(m_lds);
  assert(readTy->isIntOrIntVectorTy() || readTy->isFPOrFPVectorTy());

  Value *readPtr = m_builder->CreateGEP(m_lds->getValueType(), m_lds, {m_builder->getInt32(0), ldsOffset});

  const unsigned bitWidth = readTy->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // For 8-bit and 16-bit data type, we read them as 32-bit types from LDS. They are not packed tightly in LDS.
    unsigned numElems = readTy->isVectorTy() ? cast<FixedVectorType>(readTy)->getNumElements() : 1;

    Type *newReadTy = m_builder->getInt32Ty();
    if (readTy->isVectorTy())
      newReadTy = FixedVectorType::get(m_builder->getInt32Ty(), numElems);

    readPtr =
        m_builder->CreateBitCast(readPtr, PointerType::get(newReadTy, readPtr->getType()->getPointerAddressSpace()));
    Value *readValue = m_builder->CreateAlignedLoad(newReadTy, readPtr, Align(4));

    Type *truncTy = m_builder->getIntNTy(bitWidth);
    if (readTy->isVectorTy())
      truncTy = FixedVectorType::get(m_builder->getIntNTy(bitWidth), numElems);

    readValue = m_builder->CreateTrunc(readValue, truncTy);

    if (readTy->isFPOrFPVectorTy())
      readValue = m_builder->CreateBitCast(readValue, readTy);

    return readValue;
  }

  readPtr = m_builder->CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
  return m_builder->CreateAlignedLoad(readTy, readPtr, Align(4));
}

// =====================================================================================================================
// Write value to mesh shader LDS.
//
// @param writeValue : Value to write
// @param ldsOffset : LDS offset in dwords
void MeshTaskShader::writeValueToLds(Value *writeValue, Value *ldsOffset) {
  assert(m_lds);

  auto writeTy = writeValue->getType();
  assert(writeTy->isIntOrIntVectorTy() || writeTy->isFPOrFPVectorTy());

  Value *writePtr = m_builder->CreateGEP(m_lds->getValueType(), m_lds, {m_builder->getInt32(0), ldsOffset});

  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // For 8-bit and 16-bit data type, we write them as 32-bit types to LDS. They are not packed tightly in LDS.
    unsigned numElems = writeTy->isVectorTy() ? cast<FixedVectorType>(writeTy)->getNumElements() : 1;

    if (writeTy->isFPOrFPVectorTy()) {
      Type *castTy = m_builder->getIntNTy(bitWidth);
      if (writeTy->isVectorTy())
        castTy = FixedVectorType::get(m_builder->getIntNTy(bitWidth), numElems);

      writeValue = m_builder->CreateBitCast(writeValue, castTy);
    }

    Type *extTy = m_builder->getInt32Ty();
    if (writeTy->isVectorTy())
      extTy = FixedVectorType::get(m_builder->getInt32Ty(), numElems);

    writeValue = m_builder->CreateZExt(writeValue, extTy);

    writePtr = m_builder->CreateBitCast(
        writePtr, PointerType::get(writeValue->getType(), writePtr->getType()->getPointerAddressSpace()));
    m_builder->CreateAlignedStore(writeValue, writePtr, Align(4));
    return;
  }

  writePtr = m_builder->CreateBitCast(
      writePtr, PointerType::get(writeValue->getType(), writePtr->getType()->getPointerAddressSpace()));
  m_builder->CreateAlignedStore(writeValue, writePtr, Align(4));
}

// =====================================================================================================================
// Do atomic binary operation with the value stored in LDS.
//
// @param atomicOp : Atomic binary operation
// @param atomicValue : Value to do atomic operation
// @param ldsOffset : LDS offset in dwords
void MeshTaskShader::atomicOpWithLds(AtomicRMWInst::BinOp atomicOp, Value *atomicValue, Value *ldsOffset) {
  assert(atomicValue->getType()->isIntegerTy(32));

  // NOTE: Here, we just use LDS atomics to do ALU operations on LDS. No synchronization between threads is needed.
  Value *atomicPtr = m_builder->CreateGEP(m_lds->getValueType(), m_lds, {m_builder->getInt32(0), ldsOffset});
  m_builder->CreateAtomicRMW(atomicOp, atomicPtr, atomicValue, MaybeAlign(), AtomicOrdering::Monotonic,
                             SyncScope::SingleThread);
}

} // namespace lgc
