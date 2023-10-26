/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm-dialects/Dialect/Visitor.h"
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
// @param getPostDomTree : Function to get the post dominator tree of the given function
MeshTaskShader::MeshTaskShader(PipelineState *pipelineState,
                               PatchPreparePipelineAbi::FunctionAnalysisHandlers *analysisHandlers)
    : m_pipelineState(pipelineState), m_analysisHandlers(analysisHandlers), m_builder(pipelineState->getContext()),
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
  // +--------------+-----------------+--------------------+-------------------+-------------------+
  // | Vertex Count | Primitive Count | Barrier Completion | Flat Workgroup ID | Primitive Indices | >>>
  // +--------------+-----------------+--------------------+-------------------+-------------------+
  //       +----------------+-------------------+
  //   >>> | Vertex Outputs | Primitive Outputs |
  //       +----------------+-------------------+
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

  // Barrier completion
  ldsRegionSize = 1; // A dword corresponds to barrier completion flag (i32)
  if (ldsLayout) {
    printLdsRegionInfo("Barrier Completion", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::BarrierCompletion] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Flat workgroup ID
  if (useFlatWorkgroupId(pipelineState)) {
    ldsRegionSize = 1; // A dword corresponds to flat workgroup ID (i32)
    if (ldsLayout) {
      printLdsRegionInfo("Flat Workgroup ID", ldsOffsetInDwords, ldsRegionSize);
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
  // NOTE: For GFX11+, HW will provide workgroup ID via SGPRs. We don't need flat workgroup ID to do emulation.
  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 11)
    return false;

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
  //   Task shader main body (from API shader, lower task payload pointer)
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
  m_builder.SetInsertPointPastAllocas(entryPoint);
  initWaveThreadInfo(entryPoint);

  static auto visitor = llvm_dialects::VisitorBuilder<MeshTaskShader>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<TaskPayloadPtrOp>(&MeshTaskShader::lowerTaskPayloadPtr)
                            .add<EmitMeshTasksOp>(&MeshTaskShader::lowerEmitMeshTasks)
                            .build();
  visitor.visit(*this, *entryPoint);

  // Clear removed calls
  for (auto call : m_callsToRemove) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
  m_callsToRemove.clear();
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
  //     Write barrier completion flag to LDS (if needBarrierFlag)
  //     Write flat workgroup ID to LDS (only for GFX10.3)
  //   }
  //
  //   Barrier
  //   if (waveId < numMeshWaves) {
  //     if (threadIdInSubgroup < numMeshThreads) {
  //       Mesh shader main body (from API shader)
  //         1. Handle API barriers (if needBarrierFlag):
  //           - Flip barrier toggle (barrierToggle = !barrierToggle) when encountering each API barrier
  //           - Write barrier completion flag to LDS (barrierFlag = barrierToggle ? 0b11 : 0b10)
  //         2. Lower mesh shader specific calls:
  //           - SetMeshOutputs -> Write vertex/primitive count to LDS and send message GS_ALLOC_REQ
  //             (threadIdInSubgroup == 0)
  //           - SetMeshPrimitiveIndices -> Write primitive connectivity data to LDS
  //           - SetMeshPrimitiveCulled -> Write null primitive flag to LDS
  //           - GetMeshBuiltinInput -> Lower mesh built-in input
  //           - TaskPayloadPtr -> Transform task payload descriptor
  //           - WriteMeshVertexOutput/WriteMeshPrimitiveOutput -> Write output data to LDS
  //     }
  //
  //     Barrier (if needBarrierFlag)
  //   } else {
  //     Extra waves to add additional barriers (if needBarrierFlag):
  //     do {
  //       barrierToggle = !barrierToggle
  //       Barrier
  //
  //       Read barrierFlag from LDS:
  //         barriersCompleted = barrierFlag != 0
  //         barriersToggle = barrierFlag & 0x1
  //     } while (!barriersCompleted || barriersToggle == barrierToggle)
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

  // Determine if barrier completion flag is needed
  m_needBarrierFlag = checkNeedBarrierFlag(entryPoint);

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
  const unsigned flatWorkgroupSize =
      alignTo(m_pipelineState->enableMeshRowExport() ? numMeshThreads : primAmpFactor, waveSize);
  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        std::to_string(primAmpFactor) + std::string(",") + std::to_string(flatWorkgroupSize));

  const unsigned numWaves = flatWorkgroupSize / waveSize;
  const unsigned numMeshWaves = alignTo(numMeshThreads, waveSize) / waveSize;

  // API mesh shader entry block
  BasicBlock *apiMeshEntryBlock = &entryPoint->getEntryBlock();
  apiMeshEntryBlock->setName(".apiMeshEntry");

  // API mesh shader exit block
  BasicBlock *apiMeshExitBlock = nullptr;
  for (auto &block : *entryPoint) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      apiMeshExitBlock = &block;
      break;
    }
  }
  assert(apiMeshExitBlock);
  apiMeshExitBlock->setName(".apiMeshExit");
  auto endMeshWaveBlock = apiMeshExitBlock->splitBasicBlock(apiMeshExitBlock->getTerminator(), ".endApiMeshWave");

  // Helper to create basic block
  auto createBlock = [&](const char *blockName, BasicBlock *insertBefore = nullptr) {
    return BasicBlock::Create(entryPoint->getParent()->getContext(), blockName, entryPoint, insertBefore);
  };

  auto entryBlock = createBlock(".entry", apiMeshEntryBlock);
  auto initPrimitiveIndicesHeaderBlock = createBlock(".initPrimitiveIndicesHeader", apiMeshEntryBlock);
  auto initPrimitiveIndicesBodyBlock = createBlock(".initPrimitiveIndicesBody", apiMeshEntryBlock);
  auto endInitPrimitiveIndicesBlock = createBlock(".endInitPrimitiveIndices", apiMeshEntryBlock);

  auto writeSpecialValueBlock = createBlock(".writeSpecialValue", apiMeshEntryBlock);
  auto endWriteSpecialValueBlock = createBlock(".endWriteSpecialValue", apiMeshEntryBlock);

  auto beginMeshWaveBlock = createBlock(".beginMeshWave", apiMeshEntryBlock);

  auto beginExtraWaveBlock = createBlock(".beginExtraWave");
  auto checkMeshOutputCountBlock = createBlock(".checkMeshOutputCount");

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
    m_builder.SetInsertPoint(entryBlock);

    // Keep allocas in entry block
    while (true) {
      auto alloca = apiMeshEntryBlock->begin();
      if (alloca == apiMeshEntryBlock->end() || !isa<AllocaInst>(alloca))
        break;

      alloca->moveBefore(*entryBlock, entryBlock->end());
    }

    initWaveThreadInfo(entryPoint);

    if (m_needBarrierFlag) {
      m_barrierToggle = m_builder.CreateAlloca(m_builder.getInt1Ty(), nullptr, "barrierToggle");
      m_builder.CreateStore(m_builder.getFalse(), m_barrierToggle);
    }

    m_builder.CreateBr(initPrimitiveIndicesHeaderBlock);
  }

  // Construct ".initPrimitiveIndicesHeader" block
  PHINode *loopIndexPhi = nullptr;
  {
    m_builder.SetInsertPoint(initPrimitiveIndicesHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder.getInt32(0), entryBlock); // loopIndex = 0

      // primitiveIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder.CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                              m_builder.CreateMul(loopIndexPhi, m_builder.getInt32(waveSize)), "primitiveIndex");
    }

    if (m_gfxIp.major >= 11)
      prepareAttribRingAccess();

    auto validPrimitive =
        m_builder.CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(meshMode.outputPrimitives));
    m_builder.CreateCondBr(validPrimitive, initPrimitiveIndicesBodyBlock, endInitPrimitiveIndicesBlock);
  }

  // Construct ".initPrimitiveIndicesBody" block
  {
    m_builder.SetInsertPoint(initPrimitiveIndicesBodyBlock);

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
      auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, initPrimitiveIndicesBodyBlock);
    }

    auto ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
    auto ldsOffset = m_builder.CreateAdd(ldsStart, m_waveThreadInfo.primOrVertexIndex);

    writeValueToLds(m_builder.getInt32(0), ldsOffset);
    m_builder.CreateBr(m_pipelineState->enableMeshRowExport() ? initPrimitiveIndicesHeaderBlock
                                                              : endInitPrimitiveIndicesBlock);
  }

  // Construct ".endInitPrimitiveIndices" block
  Value *firstThreadInSubgroup = nullptr;
  {
    m_builder.SetInsertPoint(endInitPrimitiveIndicesBlock);

    firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, writeSpecialValueBlock, endWriteSpecialValueBlock);
  }

  // Construct ".writeSpecialValue" block
  {
    m_builder.SetInsertPoint(writeSpecialValueBlock);

    // NOTE: We write invalid value (~0) to vertex count as the sentinel. If API mesh shader executes
    // SetMeshOutputs, the value will be changed to a valid one. Otherwise, we know SetMeshOutputs is not be
    // executed and we must make a dummy sendmsg (GS_ALLOC_REQ) with zero vertex/primitive count.
    auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    writeValueToLds(m_builder.getInt32(InvalidValue), ldsOffset);

    // Write barrier completion flag to LDS if it is required. Otherwise, skip it.
    if (m_needBarrierFlag) {
      auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::BarrierCompletion));
      writeValueToLds(m_builder.getInt32(0), ldsOffset);
    }

    // Write flat workgroup ID to LDS if it is required. Otherwise, skip it.
    if (useFlatWorkgroupId(m_pipelineState)) {
      auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::FlatWorkgroupId));
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
      auto flatWorkgroupId = getFunctionArgument(entryPoint, entryArgIdxs.flatWorkgroupId);
      writeValueToLds(flatWorkgroupId, ldsOffset);
    }

    m_builder.CreateBr(endWriteSpecialValueBlock);
  }

  // Construct ".endWriteSpecialValue" block
  {
    m_builder.SetInsertPoint(endWriteSpecialValueBlock);

    createFenceAndBarrier();

    auto validMeshWave = m_builder.CreateICmpULT(m_waveThreadInfo.waveIdInSubgroup, m_builder.getInt32(numMeshWaves));
    // There could be no extra waves
    validMeshWave = m_builder.CreateOr(validMeshWave, m_builder.getInt1(numMeshWaves == numWaves));
    m_builder.CreateCondBr(validMeshWave, beginMeshWaveBlock, beginExtraWaveBlock);
  }

  // Construct ".beginMeshWave" block
  {
    m_builder.SetInsertPoint(beginMeshWaveBlock);

    auto validMeshThread =
        m_builder.CreateICmpULT(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(numMeshThreads));
    m_builder.CreateCondBr(validMeshThread, apiMeshEntryBlock, endMeshWaveBlock);
  }

  // Lower mesh shader main body
  lowerMeshShaderBody(apiMeshEntryBlock, apiMeshExitBlock);

  // Construct ".endMeshWave" block
  {
    m_builder.SetInsertPoint(endMeshWaveBlock);

    // NOTE: Here, we remove original return instruction from API mesh shader and continue to construct this block
    // with other instructions.
    endMeshWaveBlock->getTerminator()->eraseFromParent();

    if (m_needBarrierFlag)
      createFenceAndBarrier();

    m_builder.CreateBr(checkMeshOutputCountBlock);
  }

  // Construct ".beginExtraWave" block
  {
    m_builder.SetInsertPoint(beginExtraWaveBlock);

    if (m_needBarrierFlag) {
      //
      // do {
      //   barrierToggle != barrierToggle
      //   Barrier
      // } while (!barriersCompleted || barriersToggle == barrierToggle)
      //

      // barrierToggle = !barrierToggle
      Value *barrierToggle = m_builder.CreateLoad(m_builder.getInt1Ty(), m_barrierToggle);
      barrierToggle = m_builder.CreateNot(barrierToggle);
      m_builder.CreateStore(barrierToggle, m_barrierToggle);

      createBarrier();

      auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::BarrierCompletion));
      auto barrierFlag = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);

      // barriersNotCompleted = barrierFlag == 0
      auto barriersNotCompleted = m_builder.CreateICmpEQ(barrierFlag, m_builder.getInt32(0));
      // barriersToggle = barrierFlag & 0x1
      auto barriersToggle = m_builder.CreateAnd(barrierFlag, 0x1);
      barriersToggle = m_builder.CreateTrunc(barriersToggle, m_builder.getInt1Ty());

      // toggleEqual = barriersToggle == barrierToggle
      auto toggleEqual = m_builder.CreateICmpEQ(barriersToggle, barrierToggle);

      auto continueToAddBarriers = m_builder.CreateOr(barriersNotCompleted, toggleEqual);
      m_builder.CreateCondBr(continueToAddBarriers, beginExtraWaveBlock, checkMeshOutputCountBlock);
    } else {
      const unsigned numBarriers = m_barriers.size();
      // NOTEL: Here, we don't need barrier completion flag, but we still find API barriers. To match number of API
      // barriers, we add additional barriers in extra waves. The number is known.
      for (unsigned i = 0; i < numBarriers; ++i) {
        createBarrier();
      }
      m_builder.CreateBr(checkMeshOutputCountBlock);
    }
  }

  // Construct ".checkMeshOutputCount" block
  Value *vertexCount = nullptr;
  Value *primitiveCount = nullptr;
  {
    m_builder.SetInsertPoint(checkMeshOutputCountBlock);

    createFenceAndBarrier();

    Value *ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    vertexCount = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);
    vertexCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane,
                                            vertexCount); // Promoted to SGPR
    vertexCount->setName("vertexCount");

    ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveCount));
    primitiveCount = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);
    primitiveCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane,
                                               primitiveCount); // Promoted to SGPR
    primitiveCount->setName("primitiveCount");

    auto dummyAllocReq = m_builder.CreateICmpEQ(vertexCount, m_builder.getInt32(InvalidValue));
    m_builder.CreateCondBr(dummyAllocReq, checkDummyAllocReqBlock, checkExportPrimitiveBlock);
  }

  // Construct ".checkDummyAllocReq" block
  {
    m_builder.SetInsertPoint(checkDummyAllocReqBlock);

    m_builder.CreateCondBr(firstThreadInSubgroup, dummyAllocReqBlock, endDummyAllocReqBlock);
  }

  // Construct ".dummyAllocReq" block
  {
    m_builder.SetInsertPoint(dummyAllocReqBlock);

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder.getInt32(GsAllocReq), m_builder.getInt32(0)});
    m_builder.CreateBr(endDummyAllocReqBlock);
  }

  // Construct ".endDummyAllocReq" block
  {
    m_builder.SetInsertPoint(endDummyAllocReqBlock);

    m_builder.CreateRetVoid();
  }

  // Construct ".checkExportPrimitive" block
  {
    m_builder.SetInsertPoint(checkExportPrimitiveBlock);

    m_builder.CreateBr(exportPrimitiveHeaderBlock);
  }

  // Construct ".exportPrimitiveHeader" block
  {
    m_builder.SetInsertPoint(exportPrimitiveHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder.getInt32(0), checkExportPrimitiveBlock); // loopIndex = 0

      // primitiveIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder.CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                              m_builder.CreateMul(loopIndexPhi, m_builder.getInt32(waveSize)), "primitiveIndex");

      if (m_gfxIp.major >= 11) {
        // rowInSubgroup = waveIdInSubgroup + loopIndex
        m_waveThreadInfo.rowInSubgroup =
            m_builder.CreateAdd(m_waveThreadInfo.waveIdInSubgroup, loopIndexPhi, "rowInSubgroup");
      }
    }

    auto validPrimitive = m_builder.CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, primitiveCount);
    m_builder.CreateCondBr(validPrimitive, exportPrimitiveBodyBlock, endExportPrimitiveBlock);
  }

  // Construct ".exportPrimitiveBody" block
  {
    m_builder.SetInsertPoint(exportPrimitiveBodyBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   primitiveIndex = threadIdInSubgroup
      //   rowInSubgroup = waveIdInSubgroup
      //
      //   while (primitiveIndex < primitiveCount) {
      //     Export primitive
      //     Export primitive attributes
      //
      //     loopIndex += numWaves
      //     primitiveIndex += loopIndex * waveSize
      //     rowInSubgroup += loopIndex
      //   }
      //
      auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportPrimitiveBodyBlock);
    }

    exportPrimitive();
    m_builder.CreateBr(m_pipelineState->enableMeshRowExport() ? exportPrimitiveHeaderBlock : endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveBlock);

    m_builder.CreateBr(exportVertexHeaderBlock);
  }

  // Construct ".exportVertexHeader" block
  {
    m_builder.SetInsertPoint(exportVertexHeaderBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder.getInt32(0), endExportPrimitiveBlock); // loopIndex = 0

      // vertexIndex = threadIdInSubgroup + loopIndex * waveSize
      m_waveThreadInfo.primOrVertexIndex =
          m_builder.CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                              m_builder.CreateMul(loopIndexPhi, m_builder.getInt32(waveSize)), "vertexIndex");

      if (m_gfxIp.major >= 11) {
        // rowInSubgroup = waveIdInSubgroup + loopIndex
        m_waveThreadInfo.rowInSubgroup =
            m_builder.CreateAdd(m_waveThreadInfo.waveIdInSubgroup, loopIndexPhi, "rowInSubgroup");
      }
    }

    auto validVertex = m_builder.CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, vertexCount);
    m_builder.CreateCondBr(validVertex, exportVertexBodyBlock, endExportVertexBlock);
  }

  // Construct "exportVertexBody" block
  {
    m_builder.SetInsertPoint(exportVertexBodyBlock);

    if (m_pipelineState->enableMeshRowExport()) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   vertexIndex = threadIdInSubgroup
      //   rowInSubgroup = waveIdInSubgroup
      //
      //   while (vertexIndex < vertexCount) {
      //     Export vertex position data
      //     Export vertex attributes
      //
      //     loopIndex += numWaves
      //     vertexIndex += loopIndex * waveSize
      //     rowInSubgroup += loopIndex
      //   }
      //
      auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportVertexBodyBlock);
    }

    exportVertex();
    m_builder.CreateBr(m_pipelineState->enableMeshRowExport() ? exportVertexHeaderBlock : endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder.SetInsertPoint(endExportVertexBlock);

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, collectMeshStatsBlock, exitBlock);
  }

  // Construct ".collectMeshStats" block
  {
    m_builder.SetInsertPoint(collectMeshStatsBlock);

    collectMeshStatsInfo(entryPoint, primitiveCount);
    m_builder.CreateBr(exitBlock);
  }

  // Construct ".exit" block
  {
    m_builder.SetInsertPoint(exitBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Lower task payload pointer to buffer fat pointer.
//
// @param taskPayloadPtrOp : Call instruction op to get task payload pointer
void MeshTaskShader::lowerTaskPayloadPtr(TaskPayloadPtrOp &taskPayloadPtrOp) {
  m_builder.SetInsertPoint(&taskPayloadPtrOp);

  auto entryPoint = taskPayloadPtrOp.getFunction();

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  // 48-bit GPU address of from the buffer descriptor: dword1[15:0] + dword0
  auto descWord0 = m_builder.CreateExtractElement(payloadRingBufDesc, static_cast<uint64_t>(0));
  auto descWord1 = m_builder.CreateExtractElement(payloadRingBufDesc, 1);
  auto baseAddressLow = descWord0;
  auto baseAddressHigh = m_builder.CreateAnd(descWord1, 0xFFFF);

  Value *baseAddress = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2));
  baseAddress = m_builder.CreateInsertElement(baseAddress, baseAddressLow, static_cast<uint64_t>(0));
  baseAddress = m_builder.CreateInsertElement(baseAddress, baseAddressHigh, 1);
  baseAddress = m_builder.CreateBitCast(baseAddress, m_builder.getInt64Ty());

  baseAddress = m_builder.CreateAdd(baseAddress, m_builder.CreateZExt(payloadRingEntryOffset, m_builder.getInt64Ty()));
  baseAddress = m_builder.CreateBitCast(baseAddress, FixedVectorType::get(m_builder.getInt32Ty(), 2));

  baseAddressLow = m_builder.CreateExtractElement(baseAddress, static_cast<uint64_t>(0));
  baseAddressHigh = m_builder.CreateExtractElement(baseAddress, 1);
  baseAddressHigh = m_builder.CreateAnd(baseAddressHigh, 0xFFFF);
  descWord0 = baseAddressLow;
  descWord1 = m_builder.CreateAnd(descWord1, 0xFFFF0000);
  descWord1 = m_builder.CreateOr(descWord1, baseAddressHigh);

  payloadRingBufDesc = m_builder.CreateInsertElement(payloadRingBufDesc, descWord0, static_cast<uint64_t>(0));
  payloadRingBufDesc = m_builder.CreateInsertElement(payloadRingBufDesc, descWord1, 1);

  // Convert to fat pointer.
  auto taskPayloadPtr = m_builder.create<BufferDescToPtrOp>(payloadRingBufDesc);
  taskPayloadPtrOp.replaceAllUsesWith(taskPayloadPtr);

  if (getShaderStage(entryPoint) == ShaderStageTask)
    m_accessTaskPayload = true; // Mark this flag if task shader accesses task payload

  m_callsToRemove.push_back(&taskPayloadPtrOp);
}

// =====================================================================================================================
// Lower emit mesh tasks. Defines the dimension size of subsequent mesh shader workgroups to generate upon completion
// of the task shader workgroup.
//
// @param emitMeshTasksOp : Call instruction op to emit mesh tasks
void MeshTaskShader::lowerEmitMeshTasks(EmitMeshTasksOp &emitMeshTasksOp) {
  m_builder.SetInsertPoint(&emitMeshTasksOp);

  auto entryPoint = emitMeshTasksOp.getFunction();
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  auto groupCountX = emitMeshTasksOp.getGroupCountX();
  auto groupCountY = emitMeshTasksOp.getGroupCountY();
  auto groupCountZ = emitMeshTasksOp.getGroupCountZ();

  // Mark the flag of mesh linear dispatch from task when the group count Y and Z are both ones
  if (isa<ConstantInt>(groupCountY) && isa<ConstantInt>(groupCountZ)) {
    const unsigned constGroupCountY = cast<ConstantInt>(groupCountY)->getZExtValue();
    const unsigned constGroupCountZ = cast<ConstantInt>(groupCountZ)->getZExtValue();
    m_pipelineState->getShaderResourceUsage(ShaderStageTask)->builtInUsage.task.meshLinearDispatch =
        constGroupCountY == 1 && constGroupCountZ == 1;
  }

  auto emitMeshTasksCall = m_builder.GetInsertPoint();

  auto checkEmitMeshTasksBlock = m_builder.GetInsertBlock();
  auto emitMeshTasksBlock = checkEmitMeshTasksBlock->splitBasicBlock(emitMeshTasksCall, ".emitMeshTasks");
  auto endEmitMeshTasksBlock = emitMeshTasksBlock->splitBasicBlock(emitMeshTasksCall, ".endEmitMeshTasks");

  // Modify ".checkEmitMeshTasks" block
  {
    m_builder.SetInsertPoint(checkEmitMeshTasksBlock->getTerminator());

    if (m_accessTaskPayload) {
      // Make sure the task payload read/write access is completed
      m_builder.CreateFence(AtomicOrdering::Release, SyncScope::System);
      createBarrier();
    }

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, emitMeshTasksBlock, endEmitMeshTasksBlock);
    checkEmitMeshTasksBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".emitMeshTasks" block
  {
    m_builder.SetInsertPoint(emitMeshTasksBlock->getTerminator());

    //
    // Collect task statistics info
    //
    if (m_pipelineState->needSwMeshPipelineStats()) {
      auto &computeMode =
          m_pipelineState->getShaderModes()->getComputeShaderMode(); // Task shader is actually a compute shader
      const uint64_t numTaskThreads =
          computeMode.workgroupSizeX * computeMode.workgroupSizeY * computeMode.workgroupSizeZ;

      Value *meshPipeStatsBufPtr = m_pipelineSysValues.get(entryPoint)->getMeshPipeStatsBufPtr();
      Value *meshPipeStatsBufEntryPtr = m_builder.CreateGEP(
          m_builder.getInt8Ty(), meshPipeStatsBufPtr, m_builder.getInt32(offsetof(MeshPipeStatsEntry, numTaskThreads)));
      meshPipeStatsBufEntryPtr = m_builder.CreateBitCast(meshPipeStatsBufEntryPtr,
                                                         PointerType::get(m_builder.getInt64Ty(), ADDR_SPACE_GLOBAL));

      // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
      // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
      // move the added value to VGPR to mark it as "divergent".
      Value *valueToAdd = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2));
      valueToAdd = m_builder.CreateInsertElement(valueToAdd, convertToDivergent(m_builder.getInt32(numTaskThreads)),
                                                 static_cast<uint64_t>(0));
      valueToAdd =
          m_builder.CreateInsertElement(valueToAdd, convertToDivergent(m_builder.getInt32(numTaskThreads >> 32)), 1);
      valueToAdd = m_builder.CreateBitCast(valueToAdd, m_builder.getInt64Ty());

      m_builder.CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                                AtomicOrdering::Monotonic, SyncScope::System);
    }

    //
    // Write draw data
    //

    // Set X dimension to 0 if any of X, Y, Z dimension is 0:
    //   groupCountX = min(groupCountY, groupCountZ) == 0 ? 0 : groupCountX
    auto minGroupCountYZ =
        m_builder.CreateIntrinsic(Intrinsic::umin, groupCountY->getType(), {groupCountY, groupCountZ});
    groupCountX = m_builder.CreateSelect(m_builder.CreateICmpEQ(minGroupCountYZ, m_builder.getInt32(0)),
                                         m_builder.getInt32(0), groupCountX);

    Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();
    Value *drawDataRingEntryOffset = getDrawDataRingEntryOffset(entryPoint);

    // Draw data = <groupCountX, groupCountY, groupCountZ, readyBit>
    Value *groupCount = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 3));
    groupCount = m_builder.CreateInsertElement(groupCount, groupCountX, static_cast<uint64_t>(0));
    groupCount = m_builder.CreateInsertElement(groupCount, groupCountY, 1);
    groupCount = m_builder.CreateInsertElement(groupCount, groupCountZ, 2);

    m_builder.CreateIntrinsic(
        Intrinsic::amdgcn_raw_buffer_store, groupCount->getType(),
        {groupCount, drawDataRingBufDesc, m_builder.getInt32(0), drawDataRingEntryOffset, m_builder.getInt32(0)});

    // NOTE: Only the lowest 8 bits are for us to write.
    Value *readyBit = getDrawDataReadyBit(entryPoint);
    readyBit = m_builder.CreateZExt(readyBit, m_builder.getInt8Ty());

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_store, readyBit->getType(),
                              {readyBit, drawDataRingBufDesc, m_builder.getInt32(3 * sizeof(unsigned)),
                               drawDataRingEntryOffset, m_builder.getInt32(0)});
  }

  // Construct ".endEmitMeshTasks" block
  {
    m_builder.SetInsertPoint(endEmitMeshTasksBlock->getTerminator());

    // Currently, nothing to do
  }

  m_callsToRemove.push_back(&emitMeshTasksOp);
}

// =====================================================================================================================
// Lower set mesh outputs. Set the actual output size of the primitives and vertices that the mesh shader workgroup
// will emit.
//
// @param setMeshOutputsOp : Call instruction op to set mesh outputs
void MeshTaskShader::lowerSetMeshOutputs(SetMeshOutputsOp &setMeshOutputsOp) {
  m_builder.SetInsertPoint(&setMeshOutputsOp);

  assert(getShaderStage(setMeshOutputsOp.getFunction()) == ShaderStageMesh);

  auto vertexCount = setMeshOutputsOp.getVertexCount();
  auto primitiveCount = setMeshOutputsOp.getPrimitiveCount();

  auto setMeshOutputsCall = m_builder.GetInsertPoint();

  auto checkSetMeshOutputsBlock = m_builder.GetInsertBlock();
  auto setMeshOutputsBlock = checkSetMeshOutputsBlock->splitBasicBlock(setMeshOutputsCall, ".setMeshOutputs");
  auto endSetMeshOutputsBlock = setMeshOutputsBlock->splitBasicBlock(setMeshOutputsCall, ".endSetMeshOutputs");

  // Modify ".checkSetMeshOutputs" block
  {
    m_builder.SetInsertPoint(checkSetMeshOutputsBlock->getTerminator());

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, setMeshOutputsBlock, endSetMeshOutputsBlock);
    checkSetMeshOutputsBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".setMeshOutputs" block
  {
    m_builder.SetInsertPoint(setMeshOutputsBlock->getTerminator());

    // Promote vertex/primitive count to SGPRs
    vertexCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, vertexCount);
    primitiveCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, primitiveCount);

    // Check if vertex count or primitive count is zero. If so, set both to zero in order to disable vertex/primitive
    // exporting.
    auto zeroVertexCount = m_builder.CreateICmpEQ(vertexCount, m_builder.getInt32(0));
    auto zeroPrimitiveCount = m_builder.CreateICmpEQ(primitiveCount, m_builder.getInt32(0));
    auto hasZeroCount = m_builder.CreateOr(zeroVertexCount, zeroPrimitiveCount);
    vertexCount = m_builder.CreateSelect(hasZeroCount, m_builder.getInt32(0), vertexCount);
    primitiveCount = m_builder.CreateSelect(hasZeroCount, m_builder.getInt32(0), primitiveCount);

    // NOTE: Here, we promote vertex/primitive count to SGPRs once again because M0 implicitly used in s_sendmsg is
    // SGPR. LLVM backend has issues of handling this because it doesn't use s_cselect to translate LLVM IR select
    // instruction (which keeps the destination operand still in SGPR) and it doesn't use readfirstlane to promote
    // VGPR to SGPR for M0.
    vertexCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, vertexCount);
    primitiveCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, primitiveCount);

    // M0[10:0] = vertexCount, M0[22:12] = primitiveCount
    Value *m0 = m_builder.CreateShl(primitiveCount, 12);
    m0 = m_builder.CreateOr(m0, vertexCount);
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder.getInt32(GsAllocReq), m0});

    Value *ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexCount));
    writeValueToLds(vertexCount, ldsOffset);

    ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveCount));
    writeValueToLds(primitiveCount, ldsOffset);
  }

  // Construct ".endSetMeshOutputs" block
  {
    m_builder.SetInsertPoint(endSetMeshOutputsBlock->getTerminator());

    // Currently, nothing to do
  }

  m_callsToRemove.push_back(&setMeshOutputsOp);
}

// =====================================================================================================================
// Lower set mesh primitive indices. Set primitive indices by forming primitive connectivity data and writing it to LDS.
//
// @param setMeshPrimitiveIndicesOp : Call instruction op to set primitive indices for mesh shader
void MeshTaskShader::lowerSetMeshPrimitiveIndices(SetMeshPrimitiveIndicesOp &setMeshPrimitiveIndicesOp) {
  m_builder.SetInsertPoint(&setMeshPrimitiveIndicesOp);

  assert(getShaderStage(setMeshPrimitiveIndicesOp.getFunction()) == ShaderStageMesh);

  auto primitiveIndex = setMeshPrimitiveIndicesOp.getPrimitiveIndex();
  auto primitiveIndices = setMeshPrimitiveIndicesOp.getPrimitiveIndices();

  //
  // HW requires the primitive connectivity data has the following bit layout:
  //
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
  //
  auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  Value *primitiveData = nullptr;

  if (meshMode.outputPrimitive == OutputPrimitives::Points) {
    assert(primitiveIndices->getType() == m_builder.getInt32Ty()); // i32
    primitiveData = primitiveIndices;
  } else if (meshMode.outputPrimitive == OutputPrimitives::Lines) {
    assert(primitiveIndices->getType() == FixedVectorType::get(m_builder.getInt32Ty(), 2)); // v2i32
    Value *vertex0 = m_builder.CreateExtractElement(primitiveIndices, static_cast<uint64_t>(0));
    Value *vertex1 = m_builder.CreateExtractElement(primitiveIndices, 1);

    if (m_gfxIp.major <= 11) {
      primitiveData = m_builder.CreateShl(vertex1, 10);
      primitiveData = m_builder.CreateOr(primitiveData, vertex0);
    } else {
      llvm_unreachable("Not implemented!");
    }
  } else {
    assert(meshMode.outputPrimitive == OutputPrimitives::Triangles);
    Value *vertex0 = m_builder.CreateExtractElement(primitiveIndices, static_cast<uint64_t>(0));
    Value *vertex1 = m_builder.CreateExtractElement(primitiveIndices, 1);
    Value *vertex2 = m_builder.CreateExtractElement(primitiveIndices, 2);

    if (m_gfxIp.major <= 11) {
      primitiveData = m_builder.CreateShl(vertex2, 10);
      primitiveData = m_builder.CreateOr(primitiveData, vertex1);
      primitiveData = m_builder.CreateShl(primitiveData, 10);
      primitiveData = m_builder.CreateOr(primitiveData, vertex0);
    } else {
      llvm_unreachable("Not implemented!");
    }
  }

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder.CreateAdd(ldsStart, primitiveIndex);

  // NOTE: We first clear old primitive connectivity data and use atomic OR operation to set new data. This is because
  // the null primitive flag might be set via built-in CullPrimitive.
  static const unsigned ClearMask = (1u << 31);
  atomicOpWithLds(AtomicRMWInst::And, m_builder.getInt32(ClearMask), ldsOffset);
  atomicOpWithLds(AtomicRMWInst::Or, primitiveData, ldsOffset);

  m_callsToRemove.push_back(&setMeshPrimitiveIndicesOp);
}

// =====================================================================================================================
// Lower get mesh built-in value. Return the value of mesh built-in input.
//
// @param getMeshBuiltinInputOp : Call instruction op to return the value of mesh built-in input
void MeshTaskShader::lowerGetMeshBuiltinInput(GetMeshBuiltinInputOp &getMeshBuiltinInputOp) {
  m_builder.SetInsertPoint(&getMeshBuiltinInputOp);

  auto entryPoint = getMeshBuiltinInputOp.getFunction();
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  Value *input = PoisonValue::get(getMeshBuiltinInputOp.getType());
  auto builtin = getMeshBuiltinInputOp.getBuiltin();
  switch (builtin) {
  case BuiltInDrawIndex: {
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
    input = getFunctionArgument(entryPoint, entryArgIdxs.drawIndex);
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
      input = getFunctionArgument(entryPoint, entryArgIdxs.viewId);
    } else {
      input = m_builder.getInt32(0);
    }
    break;
  }
  case BuiltInNumWorkgroups: {
    input = getMeshNumWorkgroups();
    break;
  }
  case BuiltInWorkgroupId: {
    input = getMeshWorkgroupId();
    break;
  }
  case BuiltInLocalInvocationId: {
    input = getMeshLocalInvocationId();
    break;
  }
  case BuiltInGlobalInvocationId: {
    input = getMeshGlobalInvocationId();
    break;
  }
  case BuiltInLocalInvocationIndex: {
    input = getMeshLocalInvocationIndex();
    break;
  }
  case BuiltInSubgroupId: {
    // subgroupId = localInvocationIndex / subgroupSize
    auto localInvocationIndex = getMeshLocalInvocationIndex();
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStageMesh);
    assert(subgroupSize > 0 && subgroupSize % 32 == 0);
    input = m_builder.CreateLShr(localInvocationIndex, m_builder.getInt32(Log2_32(subgroupSize)));
    break;
  }
  case BuiltInNumSubgroups: {
    // numSubgroups = numMeshThreads / subgroupSize
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStageMesh);
    assert(subgroupSize > 0 && subgroupSize % 32 == 0);
    const unsigned numSubgroups = alignTo(numMeshThreads, subgroupSize) / subgroupSize;
    input = m_builder.getInt32(numSubgroups);
    break;
  }
  default: {
    llvm_unreachable("Unknown mesh built-in input!");
    break;
  }
  }

  assert(!isa<PoisonValue>(input));
  getMeshBuiltinInputOp.replaceAllUsesWith(input);

  m_callsToRemove.push_back(&getMeshBuiltinInputOp);
}

// =====================================================================================================================
// Lower set mesh primitive culled state. Set primitive culled state by writing the null primitive flag to LDS.
//
// @param setMeshPrimitiveIndicesOp : Call instruction op to set primitive indices for mesh shader
void MeshTaskShader::lowerSetMeshPrimitiveCulled(SetMeshPrimitiveCulledOp &setMeshPrimitiveCulledOp) {
  m_builder.SetInsertPoint(&setMeshPrimitiveCulledOp);

  assert(getShaderStage(setMeshPrimitiveCulledOp.getFunction()) == ShaderStageMesh);

  auto primitiveIndex = setMeshPrimitiveCulledOp.getPrimitiveIndex();
  auto isCulled = setMeshPrimitiveCulledOp.getIsCulled();

  //
  // HW requires the primitive connectivity data has the following bit layout:
  //   [31]    = Null primitive flag
  //   [28:20] = Index of vertex2
  //   [18:10] = Index of vertex1
  //   [8:0]   = Index of vertex0
  //
  assert(isCulled->getType()->isIntegerTy(1));

  static const unsigned NullPrimitive = (1u << 31);
  auto nullPrimitive = m_builder.CreateSelect(isCulled, m_builder.getInt32(NullPrimitive), m_builder.getInt32(0));

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder.CreateAdd(ldsStart, primitiveIndex);

  // NOTE: We first clear null primitive flag and use atomic OR operation to set new flag. This is because the
  // primitive connectivity data might be set via built-in PrimitiveXXXIndices.
  static const unsigned ClearMask = ~(1u << 31);
  atomicOpWithLds(AtomicRMWInst::And, m_builder.getInt32(ClearMask), ldsOffset);
  atomicOpWithLds(AtomicRMWInst::Or, nullPrimitive, ldsOffset);

  m_callsToRemove.push_back(&setMeshPrimitiveCulledOp);
}

// =====================================================================================================================
// Lower write mesh vertex output. Write mesh shader vertex outputs to LDS.
//
// @param writeMeshVertexOutputOp : Call instruction op to write vertex output for mesh shader
void MeshTaskShader::lowerWriteMeshVertexOutput(WriteMeshVertexOutputOp &writeMeshVertexOutputOp) {
  m_builder.SetInsertPoint(&writeMeshVertexOutputOp);

  assert(getShaderStage(writeMeshVertexOutputOp.getFunction()) == ShaderStageMesh);

  auto outputOffset = writeMeshVertexOutputOp.getOutputOffset();
  auto vertexIndex = writeMeshVertexOutputOp.getVertexIndex();
  auto outputValue = writeMeshVertexOutputOp.getOutputValue();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
  const unsigned vertexStride = 4 * resUsage->inOutUsage.outputMapLocCount; // Corresponds to vec4 output

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexOutput));
  Value *ldsOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(vertexStride));
  ldsOffset = m_builder.CreateAdd(ldsOffset, outputOffset);
  ldsOffset = m_builder.CreateAdd(ldsStart, ldsOffset);

  writeValueToLds(outputValue, ldsOffset);

  m_callsToRemove.push_back(&writeMeshVertexOutputOp);
}

// =====================================================================================================================
// Lower write mesh primitive output. Write mesh shader primitive outputs to LDS.
//
// @param writeMeshPrimitiveOutputOp : Call instruction op to write primitive output for mesh shader
void MeshTaskShader::lowerWriteMeshPrimitiveOutput(WriteMeshPrimitiveOutputOp &writeMeshPrimitiveOutputOp) {
  m_builder.SetInsertPoint(&writeMeshPrimitiveOutputOp);

  assert(getShaderStage(writeMeshPrimitiveOutputOp.getFunction()) == ShaderStageMesh);

  auto outputOffset = writeMeshPrimitiveOutputOp.getOutputOffset();
  auto primitiveIndex = writeMeshPrimitiveOutputOp.getPrimitiveIndex();
  auto outputValue = writeMeshPrimitiveOutputOp.getOutputValue();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
  const unsigned primitiveStride = 4 * resUsage->inOutUsage.perPrimitiveOutputMapLocCount; // Corresponds to vec4 output

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveOutput));
  Value *ldsOffset = m_builder.CreateMul(primitiveIndex, m_builder.getInt32(primitiveStride));
  ldsOffset = m_builder.CreateAdd(ldsOffset, outputOffset);
  ldsOffset = m_builder.CreateAdd(ldsStart, ldsOffset);

  writeValueToLds(outputValue, ldsOffset);

  m_callsToRemove.push_back(&writeMeshPrimitiveOutputOp);
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

    {
      // waveId = dispatchInfo[24:20]
      m_waveThreadInfo.waveIdInSubgroup =
          m_builder.CreateAnd(m_builder.CreateLShr(getFunctionArgument(entryPoint, entryArgIdxs.multiDispatchInfo), 20),
                              0x1F, "waveIdInSubgroup");
    }
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTask);

    m_waveThreadInfo.threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});
    if (waveSize == 64) {
      m_waveThreadInfo.threadIdInWave = m_builder.CreateIntrinsic(
          Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), m_waveThreadInfo.threadIdInWave});
    }
    m_waveThreadInfo.threadIdInWave->setName("threadIdInWave");

    m_waveThreadInfo.threadIdInSubgroup =
        m_builder.CreateAdd(m_builder.CreateMul(m_waveThreadInfo.waveIdInSubgroup, m_builder.getInt32(waveSize)),
                            m_waveThreadInfo.threadIdInWave, "threadIdInSubgroup");
  } else {
    // Mesh shader
    assert(getShaderStage(entryPoint) == ShaderStageMesh);

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder.getInt64(-1));

    // waveId = mergedWaveInfo[27:24]
    Value *mergedWaveInfo =
        getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
    m_waveThreadInfo.waveIdInSubgroup =
        m_builder.CreateAnd(m_builder.CreateLShr(mergedWaveInfo, 24), 0xF, "waveIdInSubgroup");

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageMesh);

    m_waveThreadInfo.threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});
    if (waveSize == 64) {
      m_waveThreadInfo.threadIdInWave = m_builder.CreateIntrinsic(
          Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), m_waveThreadInfo.threadIdInWave});
    }
    m_waveThreadInfo.threadIdInWave->setName("threadIdInWave");

    m_waveThreadInfo.threadIdInSubgroup =
        m_builder.CreateAdd(m_builder.CreateMul(m_waveThreadInfo.waveIdInSubgroup, m_builder.getInt32(waveSize)),
                            m_waveThreadInfo.threadIdInWave, "threadIdInSubgroup");

    m_waveThreadInfo.primOrVertexIndex =
        m_waveThreadInfo.threadIdInSubgroup; // Primitive or vertex index is initialized to thread ID in subgroup

    m_waveThreadInfo.rowInSubgroup =
        m_waveThreadInfo.waveIdInSubgroup; // Row number is initialized to wave ID in subgroup
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
      IRBuilder<>::InsertPointGuard guard(m_builder);
      m_builder.SetInsertPointPastAllocas(entryPoint);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTask)->entryArgIdxs.task;

      Value *workgroupIds[3] = {};
      if (m_gfxIp.major <= 11) {
        auto workgroupId = getFunctionArgument(entryPoint, entryArgIdxs.workgroupId);
        workgroupIds[0] = m_builder.CreateExtractElement(workgroupId, static_cast<uint64_t>(0));
        workgroupIds[1] = m_builder.CreateExtractElement(workgroupId, 1);
        workgroupIds[2] = m_builder.CreateExtractElement(workgroupId, 2);
      } else {
        llvm_unreachable("Not implemented!");
      }
      auto dispatchDims = getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);

      // flatWorkgroupId = workgroupId.z * dispatchDims.x * dispatchDims.y +
      //                   workgroupId.y * dispatchDims.x + workgroupId.x
      //                 = (workgroupId.z * dispatchDims.y + workgroupId.y) * dispatchDims.x + workgroupId.x
      auto flatWorkgroupId = m_builder.CreateMul(workgroupIds[2], m_builder.CreateExtractElement(dispatchDims, 1));
      flatWorkgroupId = m_builder.CreateAdd(flatWorkgroupId, workgroupIds[1]);
      flatWorkgroupId =
          m_builder.CreateMul(flatWorkgroupId, m_builder.CreateExtractElement(dispatchDims, static_cast<uint64_t>(0)));
      flatWorkgroupId = m_builder.CreateAdd(flatWorkgroupId, workgroupIds[0]);

      auto baseRingEntryIndex = getFunctionArgument(entryPoint, entryArgIdxs.baseRingEntryIndex);
      m_shaderRingEntryIndex = m_builder.CreateAdd(baseRingEntryIndex, flatWorkgroupId);
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
    IRBuilder<>::InsertPointGuard guard(m_builder);
    m_builder.SetInsertPoint(cast<Instruction>(payloadRingBufDesc)->getNextNode());

    // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
    Value *numPayloadRingEntries = m_builder.CreateUDiv(m_builder.CreateExtractElement(payloadRingBufDesc, 2),
                                                        m_builder.getInt32(PayloadRingEntrySize));
    // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
    Value *wrappedRingEntryIndex =
        m_builder.CreateAnd(ringEntryIndex, m_builder.CreateSub(numPayloadRingEntries, m_builder.getInt32(1)));
    m_payloadRingEntryOffset = m_builder.CreateMul(wrappedRingEntryIndex, m_builder.getInt32(PayloadRingEntrySize));
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
  Value *numDrawDataRingEntries = m_builder.CreateUDiv(m_builder.CreateExtractElement(drawDataRingBufDesc, 2),
                                                       m_builder.getInt32(DrawDataRingEntrySize));
  // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
  Value *wrappedRingEntryIndex =
      m_builder.CreateAnd(ringEntryIndex, m_builder.CreateSub(numDrawDataRingEntries, m_builder.getInt32(1)));
  return m_builder.CreateMul(wrappedRingEntryIndex, m_builder.getInt32(DrawDataRingEntrySize));
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
  Value *numDrawDataRingEntries = m_builder.CreateUDiv(m_builder.CreateExtractElement(drawDataRingBufDesc, 2),
                                                       m_builder.getInt32(DrawDataRingEntrySize));
  // readyBit = ringEntryIndex & numRingEnties != 0
  return m_builder.CreateICmpNE(m_builder.CreateAnd(ringEntryIndex, numDrawDataRingEntries), m_builder.getInt32(0));
}

// =====================================================================================================================
// Convert a i32 value to divergent one by inserting a "v_mov_b32" forcibly.
//
// @param value : Input i32 value
// @returns : A new i32 value that is considered to be divergent
Value *MeshTaskShader::convertToDivergent(Value *value) {
  assert(value->getType() == m_builder.getInt32Ty()); // Must be i32 typed
  auto inlineAsmTy = FunctionType::get(m_builder.getInt32Ty(), m_builder.getInt32Ty(), false);
  auto inlineAsm = InlineAsm::get(inlineAsmTy, "v_mov_b32 $0, $1", "=v,0", true);
  return m_builder.CreateCall(inlineAsm, value);
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

  // GFX11+ special SGPR input names
  static const std::array<std::string, NumSpecialSgprInputs> SpecialSgprInputNamesGfx11 = {
      "gsProgramAddrLow", "gsProgramAddrHigh", "mergedGroupInfo",
      "mergedWaveInfo",   "workgroupIdYX",     "workgroupIdZAndAttribRingBase",
      "flatScratchLow",   "flatScratchHigh",
  };

  ArrayRef<std::string> specialSgprInputNames;
  if (m_gfxIp.major == 10)
    specialSgprInputNames = ArrayRef<std::string>(SpecialSgprInputNamesGfx10);
  else
    specialSgprInputNames = ArrayRef<std::string>(SpecialSgprInputNamesGfx11);
  assert(specialSgprInputNames.size() == NumSpecialSgprInputs);

  // Add special SGPR inputs, prior to existing user data SGPRs
  auto int32Ty = m_builder.getInt32Ty();
  auto newEntryPoint =
      addFunctionArgs(entryPoint, nullptr, {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty},
                      specialSgprInputNames, (1u << NumSpecialSgprInputs) - 1);

  assert(entryPoint->use_empty());
  entryPoint->eraseFromParent();

  // Adjust indices of existing entry-point arguments
  auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
  entryArgIdx.drawIndex += NumSpecialSgprInputs;
  entryArgIdx.viewId += NumSpecialSgprInputs;
  entryArgIdx.dispatchDims += NumSpecialSgprInputs;
  entryArgIdx.baseRingEntryIndex += NumSpecialSgprInputs;
  entryArgIdx.pipeStatsBuf += NumSpecialSgprInputs;

  // NOTE: If flat workgroup ID is required, we have to add HW GS VGPRs. Only the VGPR5 "vertexId" will be used to
  // emulate flat workgroup ID since HW GS is configurated to have one vertex and one primitive in one input thread.
  // The "vertexId" VGPR5 will be incremented by 1 for each subgroup.
  if (useFlatWorkgroupId(m_pipelineState)) {
    static const std::array<std::string, 6> VgprInputNames = {"esGsOffset01", "esGsOffset23", "gsPrimitiveId",
                                                              "gsInstanceId", "esGsOffset45", "flatWorkgroupId"};

    entryPoint = newEntryPoint;
    newEntryPoint = addFunctionArgs(entryPoint, nullptr, {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty},
                                    VgprInputNames, 0, AddFunctionArgsAppend);

    assert(entryPoint->use_empty());
    entryPoint->eraseFromParent();

    entryArgIdx.flatWorkgroupId = newEntryPoint->arg_size() - 1; // The last argument
  }

  // NOTE: On GFX11+, the local invocation ID is provided by GE as a packed value (VGPR0), similar to the change of CS
  // on GFX11. The layout is as follow:
  //
  //   +-----------------------+-----------------------+-----------------------+
  //   | Local Invocation ID Z | Local Invocation ID Y | Local Invocation ID Z |
  //   | [29:20]               | [19:10]               | [9:0]                 |
  //   +-----------------------+-----------------------+-----------------------+
  if (m_gfxIp.major >= 11) {
    entryPoint = newEntryPoint;
    newEntryPoint = addFunctionArgs(entryPoint, nullptr, int32Ty, {"localInvocationId"}, 0, AddFunctionArgsAppend);

    assert(entryPoint->use_empty());
    entryPoint->eraseFromParent();

    entryArgIdx.localInvocationId = newEntryPoint->arg_size() - 1; // The last argument
  }

  return newEntryPoint;
}

// =====================================================================================================================
// Lower mesh shader main body by lowering mesh shader specific calls.
//
// @param apiMeshEntryBlock : API mesh shader entry block (before any mutation)
// @param apiMeshExitBlock : API mesh shader exit block (before any mutation)`
void MeshTaskShader::lowerMeshShaderBody(BasicBlock *apiMeshEntryBlock, BasicBlock *apiMeshExitBlock) {
  auto entryPoint = apiMeshEntryBlock->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  // Handle API mesh shader barrier
  if (m_needBarrierFlag) {
    // Flip barrier toggle when we encounter a API barrier
    for (auto barrier : m_barriers) {
      m_builder.SetInsertPoint(barrier);
      // barrierToggle = !barrierToggle
      Value *barrierToggle = m_builder.CreateLoad(m_builder.getInt1Ty(), m_barrierToggle);
      barrierToggle = m_builder.CreateNot(barrierToggle);
      m_builder.CreateStore(barrierToggle, m_barrierToggle);
    }

    // Store barrier completion flag according to barrier toggle
    m_builder.SetInsertPoint(apiMeshExitBlock->getTerminator());
    // barrierFlag = barrierToggle ? 0b11 : 0b10
    Value *barrierToggle = m_builder.CreateLoad(m_builder.getInt1Ty(), m_barrierToggle);
    Value *barrierFlag = m_builder.CreateSelect(barrierToggle, m_builder.getInt32(3), m_builder.getInt32(2));

    auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::BarrierCompletion));
    writeValueToLds(barrierFlag, ldsOffset);
  }

  // Lower mesh shader calls
  static auto visitor = llvm_dialects::VisitorBuilder<MeshTaskShader>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<TaskPayloadPtrOp>(&MeshTaskShader::lowerTaskPayloadPtr)
                            .add<SetMeshOutputsOp>(&MeshTaskShader::lowerSetMeshOutputs)
                            .add<SetMeshPrimitiveIndicesOp>(&MeshTaskShader::lowerSetMeshPrimitiveIndices)
                            .add<SetMeshPrimitiveCulledOp>(&MeshTaskShader::lowerSetMeshPrimitiveCulled)
                            .add<GetMeshBuiltinInputOp>(&MeshTaskShader::lowerGetMeshBuiltinInput)
                            .add<WriteMeshVertexOutputOp>(&MeshTaskShader::lowerWriteMeshVertexOutput)
                            .add<WriteMeshPrimitiveOutputOp>(&MeshTaskShader::lowerWriteMeshPrimitiveOutput)
                            .build();
  visitor.visit(*this, *entryPoint);

  // Clear removed calls
  for (auto call : m_callsToRemove) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
  m_callsToRemove.clear();
}

// =====================================================================================================================
// Export primitive (primitive connectivity data, primitive payload, and primitive attributes).
void MeshTaskShader::exportPrimitive() {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage;

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder.CreateAdd(ldsStart, m_waveThreadInfo.primOrVertexIndex);

  // The first dword is primitive connectivity data
  auto primitiveIndices = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);

  // The second dword is primitive payload, which has the following bit layout specified by HW:
  //
  //   +------------+------------+---------+----------------+----------------+------------------+
  //   | VRS Rate Y | VRS Rate X | Unused  | Viewport Index | RT Slice Index | Pipeline Prim ID |
  //   | [31:30]    | [29:28]    | [27:24] | [23:20]        | [19:17]        | [16:0]           |
  //   +------------+------------+---------+----------------+----------------+------------------+
  //
  // On GFX11, the bit layout is changed:
  //
  //   +---------------+---------+----------------+---------+----------------+
  //   | VRS Rate Enum | Unused  | Viewport Index | Unused  | RT Slice Index |
  //   | [31:28]       | [27:24] | [23:20]        | [19:13] | [12:0]         |
  //   +---------------+---------+----------------+---------+----------------+
  Value *primitivePayload = nullptr;
  Value *primitiveId = nullptr;
  if (builtInUsage.primitiveId) {
    primitiveId = readMeshBuiltInFromLds(BuiltInPrimitiveId);
    if (m_gfxIp.major < 11) {
      // [16:0] = Pipeline primitive ID
      auto primitiveIdMaskAndShift = m_builder.CreateAnd(primitiveId, 0x1FFFF);
      if (primitivePayload)
        primitivePayload = m_builder.CreateOr(primitivePayload, primitiveIdMaskAndShift);
      else
        primitivePayload = primitiveIdMaskAndShift;
    }
  }

  Value *layer = nullptr;
  if (builtInUsage.layer)
    layer = readMeshBuiltInFromLds(BuiltInLayer);

  Value *viewportIndex = nullptr;
  if (builtInUsage.viewportIndex)
    viewportIndex = readMeshBuiltInFromLds(BuiltInViewportIndex);

  Value *fsLayer = layer;
  Value *fsViewportIndex = viewportIndex;

  const bool enableMultiView = m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable;
  if (enableMultiView) {
    auto entryPoint = m_builder.GetInsertBlock()->getParent();
    const auto entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
    Value *viewId = getFunctionArgument(entryPoint, entryArgIdxs.viewId);

    // RT layer is view ID in simple mode (view index only).
    Value *layerFromViewId = viewId;
    if (m_pipelineState->getInputAssemblyState().multiView == MultiViewMode::PerView) {
      // RT layer is in the high 24 bits of view ID in per-view mode.
      layerFromViewId = m_builder.CreateLShr(viewId, m_builder.getInt32(8));
      if (layer)
        layerFromViewId = m_builder.CreateAdd(layerFromViewId, layer);
      // Viewport index is in [7:4] of view ID.
      Value *viewportIndexFromViewId =
          m_builder.CreateAnd(m_builder.CreateLShr(viewId, m_builder.getInt32(4)), m_builder.getInt32(0xF));
      if (viewportIndex)
        viewportIndexFromViewId = m_builder.CreateAdd(viewportIndexFromViewId, viewportIndex);
      viewportIndex = viewportIndexFromViewId;
    }

    layer = layerFromViewId;
  }

  if (layer) {
    // [19:17] = RT slice index (on GFX11, [12:0] = RT slice index)
    // When multi-view is enabled, the input view index is treated as the output layer.
    Value *layerMaskAndShift = nullptr;
    if (m_gfxIp.major < 11) {
      layerMaskAndShift = m_builder.CreateAnd(layer, 0x7);
      layerMaskAndShift = m_builder.CreateShl(layerMaskAndShift, 17);
    } else {
      layerMaskAndShift = m_builder.CreateAnd(layer, 0x1FFF);
    }
    if (primitivePayload)
      primitivePayload = m_builder.CreateOr(primitivePayload, layerMaskAndShift);
    else
      primitivePayload = layerMaskAndShift;
  }

  if (viewportIndex) {
    // [23:20] = Viewport index
    auto viewportIndexMaskAndShift = m_builder.CreateAnd(viewportIndex, 0xF);
    viewportIndexMaskAndShift = m_builder.CreateShl(viewportIndexMaskAndShift, 20);
    if (primitivePayload)
      primitivePayload = m_builder.CreateOr(primitivePayload, viewportIndexMaskAndShift);
    else
      primitivePayload = viewportIndexMaskAndShift;
  }

  if (builtInUsage.primitiveShadingRate) {
    // [31:28] = VRS rate
    auto primitiveShadingRate = readMeshBuiltInFromLds(BuiltInPrimitiveShadingRate);
    auto hwShadingRateMaskAndShift = convertToHwShadingRate(primitiveShadingRate);

    hwShadingRateMaskAndShift = m_builder.CreateAnd(hwShadingRateMaskAndShift, 0xF);
    hwShadingRateMaskAndShift = m_builder.CreateShl(hwShadingRateMaskAndShift, 28);

    if (primitivePayload)
      primitivePayload = m_builder.CreateOr(primitivePayload, hwShadingRateMaskAndShift);
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
  ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveOutput));
  auto primitiveStride = 4 * inOutUsage.perPrimitiveOutputMapLocCount;
  auto ldsOffsetBase = m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(primitiveStride));
  ldsOffsetBase = m_builder.CreateAdd(ldsStart, ldsOffsetBase);

  for (unsigned loc = 0; loc < inOutUsage.mesh.perPrimitiveGenericOutputMapLocCount; ++loc) {
    auto ldsOffset = m_builder.CreateAdd(ldsOffsetBase, m_builder.getInt32(4 * loc));
    auto exportValue = readValueFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), ldsOffset);

    std::array<Value *, 4> exportValues;
    for (unsigned j = 0; j < 4; ++j)
      exportValues[j] = m_builder.CreateExtractElement(exportValue, j);

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
        fsLayer = m_builder.getInt32(0);
        exportLayer = true;
      }
    }
  }

  if (exportLayer) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInLayer) > 0) {
      assert(fsLayer);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInLayer];
      primAttrExports.push_back({startLoc + exportLoc, fsLayer});
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
        fsViewportIndex = m_builder.getInt32(0);
        exportViewportIndex = true;
      }
    }
  }

  if (exportViewportIndex) {
    if (inOutUsage.mesh.perPrimitiveBuiltInExportLocs.count(BuiltInViewportIndex) > 0) {
      assert(fsViewportIndex);
      const unsigned exportLoc = inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInViewportIndex];
      primAttrExports.push_back({startLoc + exportLoc, fsViewportIndex});
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
    std::array<Value *, 4> positions = {
        m_builder.CreateExtractElement(position, static_cast<uint64_t>(0)), m_builder.CreateExtractElement(position, 1),
        m_builder.CreateExtractElement(position, 2), m_builder.CreateExtractElement(position, 3)};

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
      clipDistances.push_back(m_builder.CreateExtractElement(clipDistance, i));
  }

  SmallVector<Value *, 8> cullDistances;
  if (builtInUsage.cullDistance > 0) {
    auto cullDistance = readMeshBuiltInFromLds(BuiltInCullDistance);
    for (unsigned i = 0; i < builtInUsage.cullDistance; ++i)
      cullDistances.push_back(m_builder.CreateExtractElement(cullDistance, i));
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
    auto poison = PoisonValue::get(m_builder.getFloatTy());
    if (clipCullDistances.size() <= 4) {
      while (clipCullDistances.size() < 4) // <4 x float>
        clipCullDistances.push_back(poison);
    } else {
      while (clipCullDistances.size() < 8) // <8 x float>
        clipCullDistances.push_back(poison);
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

  bool waAtmPrecedesPos = false;
  if (m_gfxIp.major >= 11)
    waAtmPrecedesPos = m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos;

  if (!waAtmPrecedesPos)
    doExport(ExportKind::Pos, posExports);

  SmallVector<ExportInfo, 32> vertAttrExports;

  // Export vertex attributes (from generic outputs)
  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::VertexOutput));
  auto vertexStride = 4 * inOutUsage.outputMapLocCount;
  auto ldsOffsetBase = m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(vertexStride));
  ldsOffsetBase = m_builder.CreateAdd(ldsStart, ldsOffsetBase);

  for (unsigned i = 0; i < inOutUsage.mesh.genericOutputMapLocCount; ++i) {
    auto ldsOffset = m_builder.CreateAdd(ldsOffsetBase, m_builder.getInt32(4 * i));
    auto exportValue = readValueFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), ldsOffset);

    std::array<Value *, 4> exportValues = {m_builder.CreateExtractElement(exportValue, static_cast<uint64_t>(0)),
                                           m_builder.CreateExtractElement(exportValue, 1),
                                           m_builder.CreateExtractElement(exportValue, 2),
                                           m_builder.CreateExtractElement(exportValue, 3)};

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

        auto poison = PoisonValue::get(m_builder.getFloatTy());

        clipCullDistances.clear();
        for (unsigned i = 0; i < clipDistanceCount; ++i)
          clipCullDistances.push_back(clipDistances[i]);

        for (unsigned i = clipDistanceCount; i < fsBuiltInUsage.clipDistance; ++i)
          clipCullDistances.push_back(poison);

        for (unsigned i = 0; i < cullDistanceCount; ++i)
          clipCullDistances.push_back(cullDistances[i]);

        // Do array padding
        if (clipCullDistances.size() <= 4) {
          while (clipCullDistances.size() < 4) // <4 x float>
            clipCullDistances.push_back(poison);
        } else {
          while (clipCullDistances.size() < 8) // <8 x float>
            clipCullDistances.push_back(poison);
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
  if (waAtmPrecedesPos) {
    // Before the first export call of vertex position data, add s_wait_vscnt 0 to make sure the completion of all
    // attributes being written to the attribute ring buffer
    m_builder.CreateFence(AtomicOrdering::Release, SyncScope::System);

    doExport(ExportKind::Pos, posExports);
  }
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
    Value *meshPipeStatsBufEntryPtr = m_builder.CreateGEP(
        m_builder.getInt8Ty(), meshPipeStatsBufPtr, m_builder.getInt32(offsetof(MeshPipeStatsEntry, numMeshThreads)));
    meshPipeStatsBufEntryPtr =
        m_builder.CreateBitCast(meshPipeStatsBufEntryPtr, PointerType::get(m_builder.getInt64Ty(), ADDR_SPACE_GLOBAL));

    // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
    // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
    // move the added value to VGPR to mark it as "divergent".
    Value *valueToAdd = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2));
    valueToAdd = m_builder.CreateInsertElement(valueToAdd, convertToDivergent(m_builder.getInt32(numMeshThreads)),
                                               static_cast<uint64_t>(0));
    valueToAdd =
        m_builder.CreateInsertElement(valueToAdd, convertToDivergent(m_builder.getInt32(numMeshThreads >> 32)), 1);
    valueToAdd = m_builder.CreateBitCast(valueToAdd, m_builder.getInt64Ty());

    m_builder.CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                              AtomicOrdering::Monotonic, SyncScope::System);
  }

  //
  // Record numMeshPrimitives
  //
  {
    Value *meshPipeStatsBufEntryPtr =
        m_builder.CreateGEP(m_builder.getInt8Ty(), meshPipeStatsBufPtr,
                            m_builder.getInt32(offsetof(MeshPipeStatsEntry, numMeshPrimitives)));
    meshPipeStatsBufEntryPtr =
        m_builder.CreateBitCast(meshPipeStatsBufEntryPtr, PointerType::get(m_builder.getInt64Ty(), ADDR_SPACE_GLOBAL));

    assert(numMeshPrimitives->getType() == m_builder.getInt32Ty());

    // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
    // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
    // move the added value to VGPR to mark it as "divergent".
    Value *valueToAdd = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2));
    valueToAdd =
        m_builder.CreateInsertElement(valueToAdd, convertToDivergent(numMeshPrimitives), static_cast<uint64_t>(0));
    valueToAdd = m_builder.CreateInsertElement(valueToAdd, convertToDivergent(m_builder.getInt32(0)), 1);
    valueToAdd = m_builder.CreateBitCast(valueToAdd, m_builder.getInt64Ty());

    m_builder.CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
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

    auto poison = PoisonValue::get(valueTy);
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

    if (m_gfxIp.major >= 11) {
      if (kind == ExportKind::Pos || kind == ExportKind::Prim) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp_row, valueTy,
                                  {
                                      m_builder.getInt32(target + exports[i].index), // tgt
                                      m_builder.getInt32(validMask),                 // en
                                      values[0],                                     // src0
                                      values[1] ? values[1] : poison,                // src1
                                      values[2] ? values[2] : poison,                // src2
                                      values[3] ? values[3] : poison,                // src3
                                      m_builder.getInt1(exportDone),                 // done
                                      m_waveThreadInfo.rowInSubgroup,                // row number
                                  });
      } else {
        assert(kind == ExportKind::VertAttr || kind == ExportKind::PrimAttr);

        Value *valueToStore = PoisonValue::get(FixedVectorType::get(valueTy, 4));
        for (unsigned j = 0; j < 4; ++j) {
          if (values[j])
            valueToStore = m_builder.CreateInsertElement(valueToStore, values[j], j);
        }

        // ringOffset = attribRingBaseOffset + 32 * exportIndex * 16
        //            = attribRingBaseOffset + exportIndex * 512
        unsigned exportIndex = exports[i].index;
        if (kind == ExportKind::PrimAttr && m_hasNoVertexAttrib) {
          // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and
          // PRIM_EXPORT_COUNT. When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even
          // though this is not what we want. Hence, we should reserve param0 as a dummy vertex attribute and all
          // primitive attributes are moved after it.
          ++exportIndex;
        }
        auto locationOffset = m_builder.getInt32(exportIndex * SizeOfVec4);

        CoherentFlag coherent = {};
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
          coherent.bits.glc = true;
        }

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store, valueToStore->getType(),
                                  {valueToStore, m_attribRingBufDesc, m_waveThreadInfo.threadIdInSubgroup,
                                   locationOffset, m_attribRingBaseOffset, m_builder.getInt32(coherent.u32All)});
      }
    } else {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, valueTy,
                                {
                                    m_builder.getInt32(target + exports[i].index), // tgt
                                    m_builder.getInt32(validMask),                 // en
                                    values[0],                                     // src0
                                    values[1] ? values[1] : poison,                // src1
                                    values[2] ? values[2] : poison,                // src2
                                    values[3] ? values[3] : poison,                // src3
                                    m_builder.getInt1(exportDone),                 // done
                                    m_builder.getFalse(),                          // vm
                                });
    }
  }
}

// =====================================================================================================================
// Prepare attribute ring access by collecting attribute count, modifying the STRIDE field of attribute ring buffer
// descriptor, and calculating subgroup's attribute ring base offset.
void MeshTaskShader::prepareAttribRingAccess() {
  assert(m_gfxIp.major >= 11); // Must be GFX11+

  // The allocated numbers of vertex/primitive attributes are something as follow:
  //   1. Generic vertex attributes
  //   2. Vertex attributes mapped from vertex builtins
  //   3. Generic primitive attributes
  //   4. Primitive attributes mapped from primitive builtins
  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage.mesh;
  unsigned vertAttribCount = inOutUsage.genericOutputMapLocCount;
  for (auto &builtInExport : inOutUsage.builtInExportLocs) {
    const unsigned exportLoc = builtInExport.second;
    vertAttribCount = std::max(vertAttribCount, exportLoc + 1);
  }

  unsigned primAttribCount = inOutUsage.perPrimitiveGenericOutputMapLocCount;
  for (auto &perPrimitiveBuiltInExport : inOutUsage.perPrimitiveBuiltInExportLocs) {
    const unsigned exportLoc = perPrimitiveBuiltInExport.second;
    primAttribCount = std::max(primAttribCount, exportLoc + 1);
  }

  unsigned attribCount = vertAttribCount + primAttribCount;
  if (attribCount == 0)
    return; // No attribute export

  // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and PRIM_EXPORT_COUNT.
  // When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even though this is not what we
  // want. Hence, we should reserve param0 as a dummy vertex attribute.
  if (vertAttribCount == 0) {
    m_hasNoVertexAttrib = true;
    ++attribCount; // Count in this dummy vertex attribute
  }

  // attribRingBase[14:0]
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  Value *attribRingBase =
      getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
  attribRingBase = m_builder.CreateAnd(attribRingBase, 0x7FFF);
  m_attribRingBaseOffset =
      m_builder.CreateMul(attribRingBase, m_builder.getInt32(AttribGranularity), "attribRingBaseOffset");

  m_attribRingBufDesc = m_pipelineSysValues.get(entryPoint)->getAttribRingBufDesc();

  // Modify the field STRIDE of attribute ring buffer descriptor
  if (attribCount >= 2) {
    // STRIDE = WORD1[30:16], STRIDE is initialized to 16 by the driver, which is the right value for attribCount == 1.
    // We override the value if there are more attributes.
    auto descWord1 = m_builder.CreateExtractElement(m_attribRingBufDesc, 1);
    auto stride = m_builder.getInt32(attribCount * SizeOfVec4);
    if ((attribCount & 1) == 0) {
      // Clear the bit that was set in STRIDE by the driver.
      descWord1 = m_builder.CreateAnd(descWord1, ~0x3FFF0000);
    }
    descWord1 = m_builder.CreateOr(descWord1, m_builder.CreateShl(stride, 16)); // Set new STRIDE
    m_attribRingBufDesc = m_builder.CreateInsertElement(m_attribRingBufDesc, descWord1, 1);
  }
}

// =====================================================================================================================
// Get the flat workgroup ID of mesh shader.
//
// @returns : Value of flat workgroup ID
Value *MeshTaskShader::getMeshFlatWorkgroupId() {
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::FlatWorkgroupId));
  auto flatWorkgroupId = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);
  flatWorkgroupId = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane,
                                              flatWorkgroupId); // Promoted to SGPR
  flatWorkgroupId->setName("flatWorkgroupId");

  return flatWorkgroupId;
}

// =====================================================================================================================
// Get the built-in numWorkgroups of mesh shader.
//
// @returns : Value of the built-in numWorkgroups
Value *MeshTaskShader::getMeshNumWorkgroups() {
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;
  return getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);
}

// =====================================================================================================================
// Get the built-in WorkgroupId of mesh shader.
//
// @returns : Value of the built-in WorkgroupId
Value *MeshTaskShader::getMeshWorkgroupId() {
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  Value *workgroupIdX = nullptr;
  Value *workgroupIdY = nullptr;
  Value *workgroupIdZ = nullptr;

  if (m_gfxIp.major >= 11) {
    // The workgroup ID X and Y are reused via the SGPR of off-chip LDS base in NGG new fast launch mode
    Value *workgroupIdYX =
        getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase));
    // workgroupIdY = workgroupIdXY[31:16]
    workgroupIdY = m_builder.CreateAnd(m_builder.CreateLShr(workgroupIdYX, 16), 0xFFFF, "workgroupIdY");
    // workgroupIdX = workgroupIdXY[15:0]
    workgroupIdX = m_builder.CreateAnd(workgroupIdYX, 0xFFFF, "workgroupIdX");
    // workgroupIdZ = attribRingBaseAndWorkgroupIdZ[31:16]
    Value *workgroupIdZAndAttribRingBase =
        getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
    workgroupIdZ = m_builder.CreateAnd(m_builder.CreateLShr(workgroupIdZAndAttribRingBase, 16), 0xFFFF, "workgroupIdZ");
  } else {
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
    auto dispatchDimX = m_builder.CreateExtractElement(dispatchDims, static_cast<uint64_t>(0));
    auto dispatchDimY = m_builder.CreateExtractElement(dispatchDims, 1);
    auto dispatchDimXMulY = m_builder.CreateMul(dispatchDimX, dispatchDimY);

    workgroupIdZ = m_builder.CreateUDiv(flatWorkgroupId, dispatchDimXMulY);
    workgroupIdZ =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, workgroupIdZ, nullptr,
                                  "workgroupIdZ"); // Promoted to SGPR

    auto diff = m_builder.CreateMul(dispatchDimXMulY, workgroupIdZ);
    diff = m_builder.CreateSub(flatWorkgroupId, diff);
    workgroupIdY = m_builder.CreateUDiv(diff, dispatchDimX);
    workgroupIdY =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, workgroupIdY, nullptr,
                                  "workgroupIdY"); // Promoted to SGPR

    workgroupIdX = m_builder.CreateMul(dispatchDimX, workgroupIdY);
    workgroupIdX = m_builder.CreateSub(diff, workgroupIdX);
    workgroupIdX =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, workgroupIdX, nullptr,
                                  "workgroupIdX"); // Promoted to SGPR
  }

  Value *workgroupId = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 3));
  workgroupId = m_builder.CreateInsertElement(workgroupId, workgroupIdX, static_cast<uint64_t>(0));
  workgroupId = m_builder.CreateInsertElement(workgroupId, workgroupIdY, 1);
  workgroupId = m_builder.CreateInsertElement(workgroupId, workgroupIdZ, 2);
  workgroupId->setName("workgroupId");

  return workgroupId;
}

// =====================================================================================================================
// Get the built-in LocalInvocationId of mesh shader.
//
// @returns : Value of the built-in LocalInvocationId
Value *MeshTaskShader::getMeshLocalInvocationId() {
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStageMesh); // Must be mesh shader

  Value *localInvocationIdX = nullptr;
  Value *localInvocationIdY = nullptr;
  Value *localInvocationIdZ = nullptr;

  if (m_gfxIp.major >= 11) {
    // The local invocation ID is packed to VGPR0 on GFX11+ with the following layout:
    //
    //   +-----------------------+-----------------------+-----------------------+
    //   | Local Invocation ID Z | Local Invocation ID Y | Local Invocation ID Z |
    //   | [29:20]               | [19:10]               | [9:0]                 |
    //   +-----------------------+-----------------------+-----------------------+
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageMesh)->entryArgIdxs.mesh;

    Value *localInvocationId = getFunctionArgument(entryPoint, entryArgIdxs.localInvocationId);
    // localInvocationIdZ = localInvocationId[29:20]
    localInvocationIdZ = m_builder.CreateAnd(m_builder.CreateLShr(localInvocationId, 20), 0x3FF, "localInvocationIdZ");
    // localInvocationIdY = localInvocationId[19:10]
    localInvocationIdY = m_builder.CreateAnd(m_builder.CreateLShr(localInvocationId, 10), 0x3FF, "localInvocationIdY");
    // localInvocationIdX = localInvocationId[9:0]
    localInvocationIdX = m_builder.CreateAnd(localInvocationId, 0x3FF, "localInvocationIdX");
  } else {
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

    auto workgroupSizeX = m_builder.getInt32(meshMode.workgroupSizeX);
    auto workgroupSizeXMulY = m_builder.getInt32(meshMode.workgroupSizeX * meshMode.workgroupSizeY);

    localInvocationIdZ = m_builder.CreateUDiv(localInvocationIndex, workgroupSizeXMulY, "localInvocationIdZ");

    auto diff = m_builder.CreateMul(workgroupSizeXMulY, localInvocationIdZ);
    diff = m_builder.CreateSub(localInvocationIndex, diff);
    localInvocationIdY = m_builder.CreateUDiv(diff, workgroupSizeX, "localInvocationIdY");

    localInvocationIdX = m_builder.CreateMul(workgroupSizeX, localInvocationIdY);
    localInvocationIdX = m_builder.CreateSub(diff, localInvocationIdX, "localInvocationIdX");
  }

  Value *localInvocationId = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 3));
  localInvocationId = m_builder.CreateInsertElement(localInvocationId, localInvocationIdX, static_cast<uint64_t>(0));
  localInvocationId = m_builder.CreateInsertElement(localInvocationId, localInvocationIdY, 1);
  localInvocationId = m_builder.CreateInsertElement(localInvocationId, localInvocationIdZ, 2);
  localInvocationId->setName("localInvocationId");

  return localInvocationId;
}

// =====================================================================================================================
// Get the built-in LocalInvocationIndex of mesh shader.
//
// @returns : Value of the built-in LocalInvocationIndex
Value *MeshTaskShader::getMeshLocalInvocationIndex() {
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader
  return m_waveThreadInfo.threadIdInSubgroup;
}

// =====================================================================================================================
// Get the built-in GlobalInvocationId of mesh shader.
//
// @returns : Value of the built-in GlobalInvocationId
Value *MeshTaskShader::getMeshGlobalInvocationId() {
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStageMesh); // Must be mesh shader

  // globalInvocationId = workgroupId * workgroupSize + localInvocationId
  auto workgourpId = getMeshWorkgroupId();
  const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  auto workgroupSize =
      ConstantVector::get({m_builder.getInt32(meshMode.workgroupSizeX), m_builder.getInt32(meshMode.workgroupSizeY),
                           m_builder.getInt32(meshMode.workgroupSizeZ)});
  auto localInvocationId = getMeshLocalInvocationId();

  auto globalInvocationId = m_builder.CreateMul(workgourpId, workgroupSize);
  globalInvocationId = m_builder.CreateAdd(globalInvocationId, localInvocationId);
  globalInvocationId->setName("globalInvocationId");

  return globalInvocationId;
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
    readTy = FixedVectorType::get(m_builder.getFloatTy(), 4);
    break;
  case BuiltInPointSize:
    assert(builtInUsage.pointSize);
    readTy = m_builder.getFloatTy();
    break;
  case BuiltInClipDistance:
    assert(builtInUsage.clipDistance > 0);
    readTy = FixedVectorType::get(m_builder.getFloatTy(), builtInUsage.clipDistance);
    break;
  case BuiltInCullDistance:
    assert(builtInUsage.cullDistance > 0);
    readTy = FixedVectorType::get(m_builder.getFloatTy(), builtInUsage.cullDistance);
    break;
  case BuiltInPrimitiveId:
    assert(builtInUsage.primitiveId);
    readTy = m_builder.getInt32Ty();
    break;
  case BuiltInViewportIndex:
    assert(builtInUsage.viewportIndex);
    readTy = m_builder.getInt32Ty();
    break;
  case BuiltInLayer:
    assert(builtInUsage.layer);
    readTy = m_builder.getInt32Ty();
    break;
  case BuiltInPrimitiveShadingRate:
    assert(builtInUsage.primitiveShadingRate);
    readTy = m_builder.getInt32Ty();
    break;
  default:
    llvm_unreachable("Unexpected mesh shader built-in!");
    break;
  }

  Value *ldsOffset = nullptr;
  if (region == MeshLdsRegion::VertexOutput) {
    auto vertexStride = 4 * inOutUsage.outputMapLocCount;
    ldsOffset = m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(vertexStride));
  } else {
    assert(region == MeshLdsRegion::PrimitiveOutput);
    auto primitiveStride = 4 * inOutUsage.perPrimitiveOutputMapLocCount;
    ldsOffset = m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(primitiveStride));
  }
  ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(4 * location));

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(region));
  ldsOffset = m_builder.CreateAdd(ldsStart, ldsOffset);

  return readValueFromLds(readTy, ldsOffset);
}

// =====================================================================================================================
// Change primitive shading rate from API to HW-specific shading rate.
//
// @param primitiveShadingRate : Primitive shading rate from API
// @returns : HW-specific shading rate
Value *MeshTaskShader::convertToHwShadingRate(Value *primitiveShadingRate) {
  if (m_gfxIp.major >= 11) {
    // NOTE: In GFX11, the graphics pipeline is to support VRS rates till 4x4 which includes 2x4 and 4x2 along with
    // the legacy rates. And 1x4 and 4x1 are not supported, hence clamp 1x4 and 4x1 to 1x2 and 2x1 respectively.
    // The HW shading rate representations are enumerations as following:
    //
    //   SHADING_RATE_1x1  0x0
    //   SHADING_RATE_1x2  0x1
    //   SHADING_RATE_2x1  0x4
    //   SHADING_RATE_2x2  0x5
    //   SHADING_RATE_2x4  0x6
    //   SHADING_RATE_4x2  0x9
    //   SHADING_RATE_4x4  0xA
    //
    // The shading rate is mapped as follow:
    //
    //   HorizontalNone    | VerticalNone    (1x1) = 0b0000 -> 0b0000 = 0x0
    //   HorizontalNone    | Vertical2Pixels (1x2) = 0b0001 -> 0b0001 = 0x1
    //   HorizontalNone    | Vertical4Pixels (1x4) = 0b0010 -> 0b0001 = 0x1 (clamped)
    //   Horizontal2Pixels | VerticalNone    (2x1) = 0b0100 -> 0b0100 = 0x4
    //   Horizontal2Pixels | Vertical2Pixels (2x2) = 0b0101 -> 0b0101 = 0x5
    //   Horizontal2Pixels | Vertical4Pixels (2x4) = 0b0110 -> 0b0110 = 0x6
    //   Horizontal4Pixels | VerticalNone    (4x1) = 0b1000 -> 0b0100 = 0x4 (clamped)
    //   Horizontal4Pixels | Vertical2Pixels (4x2) = 0b1001 -> 0b1001 = 0x9
    //   Horizontal4Pixels | Vertical4Pixels (4x4) = 0b1010 -> 0b1010 = 0xA
    //

    enum : unsigned {
      HwShadingRate1x1 = 0x0,
      HwShadingRate1x2 = 0x1,
      HwShadingRate2x1 = 0x4,
      HwShadingRate2x2 = 0x5,
      HwShadingRate2x4 = 0x6,
      HwShadingRate4x2 = 0x9,
      HwShadingRate4x4 = 0xA,
    };

    // hwShadingRate = primitiveShadingRate & (Horizontal2Pixels | Horizontal4Pixels |
    //                                         Vertical2Pixels | Vertical4Pixels)
    auto hwShadingRate = m_builder.CreateAnd(
        primitiveShadingRate, m_builder.getInt32(ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels |
                                                 ShadingRateVertical2Pixels | ShadingRateVertical4Pixels));

    // hwShadingRate = hwShadingRate == 1x4 ? 1x2 : hwShadingRate
    Value *isRate1x4 = m_builder.CreateICmpEQ(hwShadingRate, m_builder.getInt32(ShadingRateVertical4Pixels));
    hwShadingRate = m_builder.CreateSelect(isRate1x4, m_builder.getInt32(HwShadingRate1x2), hwShadingRate);

    // hwShadingRate = hwShadingRate == 4x1 ? 2x1 : hwShadingRate
    Value *isRate4x1 = m_builder.CreateICmpEQ(hwShadingRate, m_builder.getInt32(ShadingRateHorizontal4Pixels));
    hwShadingRate = m_builder.CreateSelect(isRate4x1, m_builder.getInt32(HwShadingRate2x1), hwShadingRate);

    return hwShadingRate;
  }

  assert(m_gfxIp.isGfx(10, 3)); // Must be GFX10.3

  // NOTE: The shading rates have different meanings in HW and LGC interface. GFX10.3 HW supports 2-pixel mode
  // and 4-pixel mode is not supported. But the spec requires us to accept unsupported rates and clamp them to
  // maxFragmentSize of HW. The mapping is therefore as follow:
  //
  //   VRS rate X: MaskNone -> 0b00, Horizontal2Pixels | Horizontal4Pixels -> 0b01
  //   VRS rate Y: MaskNone -> 0b00, Vertical2Pixels | Vertical4Pixels -> 0b01
  //
  // hwXRate = (primitiveShadingRate & (Horizontal2Pixels | Horizontal4Pixels)) ? 0x1 : 0x0
  Value *xRate2Pixels = m_builder.CreateAnd(
      primitiveShadingRate, m_builder.getInt32(ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels));
  xRate2Pixels = m_builder.CreateICmpNE(xRate2Pixels, m_builder.getInt32(0));
  Value *hwXRate = m_builder.CreateSelect(xRate2Pixels, m_builder.getInt32(1), m_builder.getInt32(0));

  // hwYRate = (primitiveShadingRate & (Vertical2Pixels | Vertical4Pixels)) ? 0x1 : 0x0
  Value *yRate2Pixels = m_builder.CreateAnd(
      primitiveShadingRate, m_builder.getInt32(ShadingRateVertical2Pixels | ShadingRateVertical4Pixels));
  yRate2Pixels = m_builder.CreateICmpNE(yRate2Pixels, m_builder.getInt32(0));
  Value *hwYRate = m_builder.CreateSelect(yRate2Pixels, m_builder.getInt32(1), m_builder.getInt32(0));

  // hwShadingRate = (hwYRate << 2) | hwXRate
  auto hwShadingRate = m_builder.CreateShl(hwYRate, 2);
  hwShadingRate = m_builder.CreateOr(hwShadingRate, hwXRate);

  return hwShadingRate;
}

// =====================================================================================================================
// Check if barrier completion flag is needed. Barrier completion flag is to address this case:
//
//   ...
//   if (threadId < numMeshThreads) {
//     Run API mesh shader (contains API barriers)
//     ...
//     Barrier
//     Or
//     if (Uniform condition)
//       Barrier
//   }
//
//   Barrier (Post-API)
//   ...
//
// There are extra waves that will not run API mesh shader (just to export vertices and primitives as post-API
// mesh shader processing) and the API mesh shader contains API barriers by calling barrier(). As a result, the
// extra waves will be out of sync because when API mesh shader waves hit the API barriers, the extra waves
// will hit the post-API barrier. The extra waves are then out of sync after that. The solution idea is to add
// additional barriers for extra waves according to the hit number of API barriers, making them matching to
// avoid out-of-sync problems. There are two cases:
//
//   1. Barriers are all placed in the entry-point
//   For such cases, we just collected all used API barriers. In extra wave, we add equal number of barriers statically
//   and the number is known from previous collecting.
//
//   2. Some of barriers are placed in uniform control flow
//   For such cases, the blocks where API barriers are placed don't post-dominate the entry block or the block is
//   contained by a cycle (loop). We have to add dynamical barrier handling. The processing is something like this:
//
//   barrierToggle = false
//   Write 0 to barrier completion flag in LDS
//   ...
//   if (API mesh waves) {
//     if (API mesh threads) {
//       ...
//       barrierToggle = !barrierToggle (Flip the toggle)
//       API barrier
//       ...
//       barrierFlag = barrierToggle ? 3 : 2 (Before API mesh shader completion)
//       Write barrierFlag to LDS
//     }
//     Barrier (Sync the completion of API mesh waves)
//   } else {
//     do {
//       barrierToggle = !barrierToggle (Flip the toggle)
//       Barrier
//
//       Read barrierFlag from LDS
//       barrierCompleted = barrierFlag != 0
//       barriersToggle = barrierFlag & 0x1
//     } while (!barrierCompleted || barriersToggle == barrierToggle)
//   }
//   ...
//
//   The barrier completion flag has 2 bits: bits[1] indicates if all API barriers are completed, bits[0] indicates the
//   toggle flipping in API mesh waves. The toggle in extra waves should not be equal to the toggle in API mesh waves
//   because we have an extra barrier in API mesh waves to sync their completion.
//
// @param entryPoint : Entry-point of mesh shader
// @returns : Value indicating whether barrier completion flag is needed
bool MeshTaskShader::checkNeedBarrierFlag(Function *entryPoint) {
  if (m_pipelineState->enableMeshRowExport())
    return false; // Not needed if row export is enable

  const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  const unsigned numThreads =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.primAmpFactor;
  assert(numThreads >= numMeshThreads);

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageMesh);
  const unsigned numMeshWaves = alignTo(numMeshThreads, waveSize) / waveSize;
  const unsigned numWaves = alignTo(numThreads, waveSize) / waveSize;
  if (numWaves == numMeshWaves)
    return false; // Wave number to run API mesh shader is equal to actual wave number to run HW mesh shader (HW GS)

  assert(getShaderStage(entryPoint) == ShaderStageMesh);
  auto module = entryPoint->getParent();
  for (auto &func : module->functions()) {
    if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_s_barrier) {
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);
        if (call->getParent()->getParent() == entryPoint)
          m_barriers.push_back(call);
      }
    }
  }

  // API mesh shader contains no barriers
  if (m_barriers.empty())
    return false;

  auto &postDomTree = m_analysisHandlers->getPostDomTree(*entryPoint);
  auto &cycleInfo = m_analysisHandlers->getCycleInfo(*entryPoint);
  auto &entryBlock = entryPoint->getEntryBlock();
  for (auto barrier : m_barriers) {
    auto barrierBlock = barrier->getParent();
    if (!postDomTree.dominates(barrierBlock, &entryBlock) || cycleInfo.getCycleDepth(barrierBlock) > 0) {
      // NOTE: If the block where the API barrier is placed doesn't post-dominates the entry block or the block is
      // contained within a cycle, we have to switch to dynamical barrier handling.
      return true;
    }
  }

  return false;
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

  Value *readPtr = m_builder.CreateGEP(m_lds->getValueType(), m_lds, {m_builder.getInt32(0), ldsOffset});

  const unsigned bitWidth = readTy->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // For 8-bit and 16-bit data type, we read them as 32-bit types from LDS. They are not packed tightly in LDS.
    unsigned numElems = readTy->isVectorTy() ? cast<FixedVectorType>(readTy)->getNumElements() : 1;

    Type *newReadTy = m_builder.getInt32Ty();
    if (readTy->isVectorTy())
      newReadTy = FixedVectorType::get(m_builder.getInt32Ty(), numElems);

    readPtr =
        m_builder.CreateBitCast(readPtr, PointerType::get(newReadTy, readPtr->getType()->getPointerAddressSpace()));
    Value *readValue = m_builder.CreateAlignedLoad(newReadTy, readPtr, Align(4));

    Type *truncTy = m_builder.getIntNTy(bitWidth);
    if (readTy->isVectorTy())
      truncTy = FixedVectorType::get(m_builder.getIntNTy(bitWidth), numElems);

    readValue = m_builder.CreateTrunc(readValue, truncTy);

    if (readTy->isFPOrFPVectorTy())
      readValue = m_builder.CreateBitCast(readValue, readTy);

    return readValue;
  }

  readPtr = m_builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
  return m_builder.CreateAlignedLoad(readTy, readPtr, Align(4));
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

  Value *writePtr = m_builder.CreateGEP(m_lds->getValueType(), m_lds, {m_builder.getInt32(0), ldsOffset});

  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // For 8-bit and 16-bit data type, we write them as 32-bit types to LDS. They are not packed tightly in LDS.
    unsigned numElems = writeTy->isVectorTy() ? cast<FixedVectorType>(writeTy)->getNumElements() : 1;

    if (writeTy->isFPOrFPVectorTy()) {
      Type *castTy = m_builder.getIntNTy(bitWidth);
      if (writeTy->isVectorTy())
        castTy = FixedVectorType::get(m_builder.getIntNTy(bitWidth), numElems);

      writeValue = m_builder.CreateBitCast(writeValue, castTy);
    }

    Type *extTy = m_builder.getInt32Ty();
    if (writeTy->isVectorTy())
      extTy = FixedVectorType::get(m_builder.getInt32Ty(), numElems);

    writeValue = m_builder.CreateZExt(writeValue, extTy);

    writePtr = m_builder.CreateBitCast(
        writePtr, PointerType::get(writeValue->getType(), writePtr->getType()->getPointerAddressSpace()));
    m_builder.CreateAlignedStore(writeValue, writePtr, Align(4));
    return;
  }

  writePtr = m_builder.CreateBitCast(
      writePtr, PointerType::get(writeValue->getType(), writePtr->getType()->getPointerAddressSpace()));
  m_builder.CreateAlignedStore(writeValue, writePtr, Align(4));
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
  Value *atomicPtr = m_builder.CreateGEP(m_lds->getValueType(), m_lds, {m_builder.getInt32(0), ldsOffset});
  m_builder.CreateAtomicRMW(atomicOp, atomicPtr, atomicValue, MaybeAlign(), AtomicOrdering::Monotonic,
                            SyncScope::SingleThread);
}

// =====================================================================================================================
// Create both LDS fence and barrier to guarantee the synchronization of LDS operations.
void MeshTaskShader::createFenceAndBarrier() {
  SyncScope::ID syncScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");
  m_builder.CreateFence(AtomicOrdering::Release, syncScope);
  createBarrier();
  m_builder.CreateFence(AtomicOrdering::Acquire, syncScope);
}

// =====================================================================================================================
// Create LDS barrier to guarantee the synchronization of LDS operations.
void MeshTaskShader::createBarrier() {

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
}

} // namespace lgc
