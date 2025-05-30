/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MeshTaskShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::MeshTaskShader.
 ***********************************************************************************************************************
 */
#include "MeshTaskShader.h"
#include "ShaderMerger.h"
#include "lgc/Debug.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/lowering/MutateEntryPoint.h"
#include "lgc/util/BufferResource.h"
#include "lgc/util/WorkgroupLayout.h"
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
                               PreparePipelineAbi::FunctionAnalysisHandlers *analysisHandlers)
    : m_pipelineState(pipelineState), m_analysisHandlers(analysisHandlers), m_builder(pipelineState->getContext()),
      m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()) {
  assert(pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  m_pipelineSysValues.initialize(m_pipelineState);
}

// =====================================================================================================================
// Layout mesh shader LDS if 'ldsLayout' is specified and calculate the required total LDS size (in dwords).
//
// @param pipelineState : Pipeline state
// @param entryPoint : Entry-point of mesh shader
// @param ldsLayout : Mesh shader LDS layout (could be null)
// @param outputsLayout : Mesh shader outputs layout (could be null)
unsigned MeshTaskShader::layoutMeshShaderLds(PipelineState *pipelineState, Function *entryPoint,
                                             MeshLdsLayout *ldsLayout, MeshOutputsLayout *outputsLayout) {
  if (!pipelineState->hasShaderStage(ShaderStage::Mesh))
    return 0; // Mesh shader absent (standalone compiler tries to compile a single task shader)

  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);                           // Must be mesh shader
  assert(pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  //
  // The LDS layout of mesh shader is something as follow (consists of two main parts):
  //
  // 1. Internal mesh LDS:
  //
  // +--------------------+--------------------+-------------------+-------------------+
  // | Mesh Output Counts | Barrier Completion | Flat Workgroup ID | Primitive Indices | >>>
  // +--------------------+--------------------+-------------------+-------------------+
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
  assert(meshMode.outputVertices <= NggMaxThreadsPerSubgroup);
  assert(meshMode.outputPrimitives <= NggMaxThreadsPerSubgroup);

  bool outputsToAllocas = meshOutputsToAllocas(pipelineState, entryPoint);
  if (ldsLayout) {
    assert(outputsLayout);
    outputsLayout->outputsToAllocas = outputsToAllocas;
  }

  const auto resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Mesh);
  const auto nextStage = pipelineState->getNextShaderStage(ShaderStage::Mesh);

  unsigned meshLdsSizeInDwords = 0;
  unsigned ldsOffsetInDwords = 0;
  unsigned ldsRegionSize = 0;

  auto printLdsRegionInfo = [=](const char *regionName, unsigned regionOffset, unsigned regionSize) {
    if (regionSize == 0)
      return;
    LLPC_OUTS(
        format("%-30s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32 "\n", regionName, regionOffset, regionSize));
  };

  auto printOutputLayoutInfo = [=](unsigned location, unsigned numComponents, unsigned relativeOffset,
                                   unsigned exportSlot, BuiltInKind forBuiltIn) {
    if (numComponents > 4) {
      LLPC_OUTS(format("-- location = %u-%u, components = %u, offset = %u", location, location + 1, numComponents,
                       relativeOffset));
    } else {
      LLPC_OUTS(format("-- location = %u, components = %u, offset = %u", location, numComponents, relativeOffset));
    }

    if (exportSlot != InvalidValue)
      LLPC_OUTS(format(", export = %u", exportSlot));

    if (forBuiltIn != InvalidValue)
      LLPC_OUTS(" (builtin = " << PipelineState::getBuiltInName(forBuiltIn) << ")");

    LLPC_OUTS("\n");
  };

  if (ldsLayout) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC mesh shader LDS region info (in dwords) and general info\n\n");
  }

  // Mesh output counts
  ldsRegionSize = 2; //  Two dwords correspond to vertex/primitive count (i32)
  if (ldsLayout) {
    // Make sure this region starts from zero offset in order to use 64-bit LDS access (8-byte alignment) later on.
    assert(ldsOffsetInDwords == 0);
    printLdsRegionInfo("Mesh Output Counts", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::MeshOutputCounts] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
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
  ldsRegionSize =
      outputsToAllocas ? 0 : meshMode.outputPrimitives; // Each dword corresponds to primitive connectivity data (i32)
  if (ldsLayout) {
    printLdsRegionInfo("Primitive Indices", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::PrimitiveIndices] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);
    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Vertex outputs
  auto &vertexOutputComponents = resUsage->inOutUsage.mesh.vertexOutputComponents;
  unsigned vertexStride = 0;
  if (!outputsToAllocas) {
    for (auto &vertexOutput : vertexOutputComponents) {
      const auto numComponents = vertexOutput.second.first;
      vertexStride += numComponents; // Calculate total number of components of vertex outputs
    }
  }

  ldsRegionSize = vertexStride * meshMode.outputVertices;
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Outputs", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::VertexOutput] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);

    assert(outputsLayout);
    outputsLayout->vertexStride = vertexStride;

    unsigned offsetInVertex = 0;
    unsigned exportSlot = 0;
    unsigned exportCount = 0;

    for (auto &vertexOutput : vertexOutputComponents) {
      const auto location = vertexOutput.first;
      const auto &[numComponents, forBuiltIn] = vertexOutput.second;

      if (!outputsToAllocas) {
        outputsLayout->offsetsInVertex[location] = offsetInVertex; // Map output locations to relative offsets in vertex
        offsetInVertex += numComponents;
      }

      if (forBuiltIn == InvalidValue) {
        // Only consider vertex generic outputs, vertex built-ins will be handled later on
        if (nextStage == ShaderStage::Fragment) {
          // Input/output matching must have been done in resource collecting pass, just use the location as export slot
          outputsLayout->vertexGenericExports[location] = location;
          exportCount = std::max(exportCount, location + 1);
          if (numComponents > 4) {
            outputsLayout->vertexGenericExports[location + 1] = location + 1;
            exportCount = std::max(exportCount, location + 2);
          }
        } else {
          // If next stage is missing, we try to assign continuous export slots
          assert(!nextStage);

          outputsLayout->vertexGenericExports[location] = exportSlot++;
          ++exportCount;
          if (numComponents > 4) {
            outputsLayout->vertexGenericExports[location + 1] = exportSlot++;
            ++exportCount;
          }
        }
      }
    }

    // Consider those special outputs mapped from vertex built-ins
    if (nextStage == ShaderStage::Fragment) {
      const auto fsResUsage = pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
      const auto &fsBuiltInUsage = fsResUsage->builtInUsage.fs;
      auto &fsInOutUsage = fsResUsage->inOutUsage;

      if (fsBuiltInUsage.clipDistance > 0 || fsBuiltInUsage.cullDistance > 0) {
        if (fsBuiltInUsage.clipDistance > 0) {
          assert(fsInOutUsage.builtInInputLocMap.count(BuiltInClipDistance) > 0);
          exportSlot = fsInOutUsage.builtInInputLocMap[BuiltInClipDistance];
          outputsLayout->vertexBuiltInExports[BuiltInClipDistance] = exportSlot;
        }

        if (fsBuiltInUsage.cullDistance > 0) {
          assert(fsInOutUsage.builtInInputLocMap.count(BuiltInCullDistance) > 0);
          exportSlot = fsInOutUsage.builtInInputLocMap[BuiltInCullDistance];
          outputsLayout->vertexBuiltInExports[BuiltInCullDistance] = exportSlot;
        }

        unsigned startSlot = InvalidValue;
        if (fsBuiltInUsage.clipDistance > 0) {
          startSlot = outputsLayout->vertexBuiltInExports[BuiltInClipDistance];
        } else {
          assert(fsBuiltInUsage.cullDistance > 0);
          startSlot = outputsLayout->vertexBuiltInExports[BuiltInCullDistance];
        }
        exportCount = std::max(exportCount,
                               startSlot + ((fsBuiltInUsage.clipDistance + fsBuiltInUsage.cullDistance > 4) ? 2 : 1));
      }
    } else {
      assert(!nextStage);

      const auto &builtInUsage = resUsage->builtInUsage.mesh;
      if (builtInUsage.clipDistance > 0 || builtInUsage.cullDistance > 0) {
        unsigned startSlot = exportSlot;

        if (builtInUsage.clipDistance > 0)
          outputsLayout->vertexBuiltInExports[BuiltInClipDistance] = startSlot;

        if (builtInUsage.cullDistance > 0) {
          if (builtInUsage.clipDistance >= 4)
            ++startSlot;
          outputsLayout->vertexBuiltInExports[BuiltInCullDistance] = startSlot;
        }

        exportSlot += (builtInUsage.clipDistance + builtInUsage.cullDistance > 4) ? 2 : 1;
        exportCount += (builtInUsage.clipDistance + builtInUsage.cullDistance > 4) ? 2 : 1;
      }
    }
    outputsLayout->vertexExportCount = exportCount;

    ldsOffsetInDwords += ldsRegionSize;
  }
  meshLdsSizeInDwords += ldsRegionSize;

  // Primitive outputs
  auto &primitiveOutputComponents = resUsage->inOutUsage.mesh.primitiveOutputComponents;
  unsigned primitiveStride = 0;
  if (!outputsToAllocas) {
    for (auto &primitiveOutput : primitiveOutputComponents) {
      const auto numComponents = primitiveOutput.second.first;
      primitiveStride += numComponents; // Calculate total number of components of primitive outputs
    }
  }

  ldsRegionSize = primitiveStride * meshMode.outputPrimitives;
  if (ldsLayout) {
    printLdsRegionInfo("Primitive Outputs", ldsOffsetInDwords, ldsRegionSize);
    (*ldsLayout)[MeshLdsRegion::PrimitiveOutput] = std::make_pair(ldsOffsetInDwords, ldsRegionSize);

    assert(outputsLayout);
    outputsLayout->primitiveStride = primitiveStride;

    bool hasDummyVertexAttrib = false;
    if (!pipelineState->attributeThroughExport()) {
      if (outputsLayout->vertexExportCount == 0) {
        // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and
        // PRIM_EXPORT_COUNT. When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even
        // though this is not what we want. Hence, we should reserve param0 as a dummy vertex attribute.
        hasDummyVertexAttrib = true;
      }
    }

    unsigned offsetInPrimitive = 0;
    const unsigned startSlot = hasDummyVertexAttrib ? 1 : outputsLayout->vertexExportCount;
    unsigned exportSlot = startSlot;
    unsigned exportCount = 0;

    for (auto &primitiveOutput : primitiveOutputComponents) {
      const auto location = primitiveOutput.first;
      const auto &[numComponents, forBuiltIn] = primitiveOutput.second;

      if (!outputsToAllocas) {
        outputsLayout->offsetsInPrimitive[location] =
            offsetInPrimitive; // Map output locations to relative offsets in primitive
        offsetInPrimitive += numComponents;
      }

      if (forBuiltIn == InvalidValue) {
        // Only consider primitive generic outputs, primitive built-ins will be handled later on
        if (nextStage == ShaderStage::Fragment) {
          // Input/output matching must have been done in resource collecting pass, just use the location as export slot
          outputsLayout->primitiveGenericExports[location] = startSlot + location;
          exportCount = std::max(exportCount, location + 1);
          if (numComponents > 4) {
            outputsLayout->primitiveGenericExports[location + 1] = startSlot + location + 1;
            exportCount = std::max(exportCount, location + 2);
          }
        } else {
          // If next stage is missing, we try to assign continuous export slots
          assert(!nextStage);

          outputsLayout->primitiveGenericExports[location] = exportSlot++;
          ++exportCount;
          if (numComponents > 4) {
            outputsLayout->primitiveGenericExports[location + 1] = exportSlot++;
            ++exportCount;
          }
        }
      }
    }

    // Consider those special outputs mapped from primitive built-ins
    if (nextStage == ShaderStage::Fragment) {
      // Built-in matching must have been done in resource collecting pass, just use the location as export slot
      const auto fsResUsage = pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
      const auto &fsBuiltInUsage = fsResUsage->builtInUsage.fs;
      auto &fsInOutUsage = fsResUsage->inOutUsage;

      if (fsBuiltInUsage.primitiveId) {
        assert(fsInOutUsage.perPrimitiveBuiltInInputLocMap.count(BuiltInPrimitiveId) > 0);
        const unsigned location = fsInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInPrimitiveId];
        outputsLayout->primitiveBuiltInExports[BuiltInPrimitiveId] = startSlot + location;
        exportCount = std::max(exportCount, location + 1);
      }

      if (fsBuiltInUsage.layer) {
        assert(fsInOutUsage.perPrimitiveBuiltInInputLocMap.count(BuiltInLayer) > 0);
        const unsigned location = fsInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInLayer];
        outputsLayout->primitiveBuiltInExports[BuiltInLayer] = startSlot + location;
        exportCount = std::max(exportCount, location + 1);
      }

      if (fsBuiltInUsage.viewportIndex) {
        assert(fsInOutUsage.perPrimitiveBuiltInInputLocMap.count(BuiltInViewportIndex) > 0);
        const unsigned location = fsInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInViewportIndex];
        outputsLayout->primitiveBuiltInExports[BuiltInViewportIndex] = startSlot + location;
        exportCount = std::max(exportCount, location + 1);
      }
    } else {
      assert(!nextStage);

      const auto &builtInUsage = resUsage->builtInUsage.mesh;
      if (builtInUsage.primitiveId) {
        outputsLayout->primitiveBuiltInExports[BuiltInPrimitiveId] = exportSlot++;
        ++exportCount;
      }

      if (builtInUsage.layer) {
        outputsLayout->primitiveBuiltInExports[BuiltInLayer] = exportSlot++;
        ++exportCount;
      }

      if (builtInUsage.viewportIndex) {
        outputsLayout->primitiveBuiltInExports[BuiltInViewportIndex] = exportSlot++;
        ++exportCount;
      }
    }
    outputsLayout->primitiveExportCount = exportCount;

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
    assert(meshSharedVar->getAlignment() % 4 == 0); // Must be multiple of 1 dword
    const auto sizeInBytes =
        meshSharedVar->getParent()->getDataLayout().getTypeAllocSize(meshSharedVar->getValueType());
    assert(sizeInBytes % 4 == 0); // Must be multiple of 4
    sharedVarLdsSizeInDwords += sizeInBytes / 4;
  }

  if (ldsLayout) {
    // Setup internal mesh LDS
    getOrCreateMeshLds(entryPoint->getParent(), meshLdsSizeInDwords);

    LLPC_OUTS("\n");
    LLPC_OUTS("Internal Mesh LDS = " << meshLdsSizeInDwords << " dwords\n");
    LLPC_OUTS("Shared Variable LDS = " << sharedVarLdsSizeInDwords << " dwords\n");
    LLPC_OUTS("Total LDS = " << meshLdsSizeInDwords + sharedVarLdsSizeInDwords << " dwords\n");

    if (!outputsLayout->offsetsInVertex.empty()) {
      LLPC_OUTS("\nVertex Outputs Layout (stride = " << outputsLayout->vertexStride
                                                     << ", exports = " << outputsLayout->vertexExportCount << "):\n");
      for (auto &vertexOutput : outputsLayout->offsetsInVertex) {
        const auto &[location, offsetInVertex] = vertexOutput;
        const auto &[numComponents, forBuiltIn] = vertexOutputComponents[location];
        unsigned exportSlot = InvalidValue;
        if (forBuiltIn != InvalidValue) {
          if (outputsLayout->vertexBuiltInExports.count(forBuiltIn) > 0)
            exportSlot = outputsLayout->vertexBuiltInExports[forBuiltIn];
        } else {
          exportSlot = outputsLayout->vertexGenericExports[location];
        }
        printOutputLayoutInfo(location, numComponents, offsetInVertex, exportSlot, forBuiltIn);
      }
    }

    if (!outputsLayout->offsetsInPrimitive.empty()) {
      LLPC_OUTS("\nPrimitive outputs layout (stride = " << outputsLayout->primitiveStride << ", exports = "
                                                        << outputsLayout->primitiveExportCount << "):\n");
      for (auto &primitiveOutput : outputsLayout->offsetsInPrimitive) {
        const auto &[location, offsetInPrimitive] = primitiveOutput;
        const auto &[numComponents, forBuiltIn] = primitiveOutputComponents[location];
        unsigned exportSlot = InvalidValue;
        if (forBuiltIn != InvalidValue) {
          if (outputsLayout->primitiveBuiltInExports.count(forBuiltIn) > 0)
            exportSlot = outputsLayout->primitiveBuiltInExports[forBuiltIn];
        } else {
          exportSlot = outputsLayout->primitiveGenericExports[location];
        }
        printOutputLayoutInfo(location, numComponents, offsetInPrimitive, exportSlot, forBuiltIn);
      }
    }

    LLPC_OUTS("\n");
    LLPC_OUTS("RowExport = " << (usesRowExport(pipelineState) ? "true" : "false") << "\n");
    LLPC_OUTS("OutputsToAllocas = " << (outputsLayout->outputsToAllocas ? "true" : "false") << "\n");
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
      LLPC_OUTS("Shared Variables:\n");
      for (auto meshSharedVar : meshSharedVars) {
        assert(meshSharedVar->getAlignment() % 4 == 0); // Must be multiple of 1 dword
        const auto sizeInBytes =
            meshSharedVar->getParent()->getDataLayout().getTypeAllocSize(meshSharedVar->getValueType());
        assert(sizeInBytes % 4 == 0); // Must be multiple of 4
        const auto sizeInDwords = sizeInBytes / 4;

        LLPC_OUTS("-- name = " << meshSharedVar->getName() << ", type = " << getTypeName(meshSharedVar->getValueType())
                               << ", size (in dwords) = " << sizeInDwords << "\n");
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

  const auto &builtInUsage = pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;
  return builtInUsage.workgroupId || builtInUsage.globalInvocationId;
}

// =====================================================================================================================
// Check whether we actually use row export for mesh shader.
//
// @param pipelineState : Pipeline state
// @returns : Whether row export can be actually used.
bool MeshTaskShader::usesRowExport(PipelineState *pipelineState) {
  if (!pipelineState->enableMeshRowExport())
    return false; // Not enabled

  const auto &meshMode = pipelineState->getShaderModes()->getMeshShaderMode();

  const unsigned waveSize = pipelineState->getShaderWaveSize(ShaderStage::Mesh);
  const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  const unsigned numExportThreads = std::max(meshMode.outputPrimitives, meshMode.outputVertices);

  // If we have enough threads after HW GS amplification to export primitives/vertices, row export is actually not used.
  if (alignTo(numExportThreads, waveSize) <= alignTo(numMeshThreads, waveSize))
    return false;

  return true;
}

// =====================================================================================================================
// Check whether mesh outputs can be written to allocas without through LDS.
//
// @param pipelineState : Pipeline state
// @param entryPoint : Entry-point of mesh shader
// @returns : Whether mesh outputs can be written to allocas
bool MeshTaskShader::meshOutputsToAllocas(PipelineState *pipelineState, Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

  const auto &meshMode = pipelineState->getShaderModes()->getMeshShaderMode();
  const bool linearDispatch = meshMode.workgroupSizeY == 1 && meshMode.workgroupSizeZ == 1;

  std::function<bool(Value *)> fromLocalInvocationIndex = [&](Value *primOrVertexIndex) -> bool {
    if (auto getMeshBuiltInInputOp = dyn_cast<GetMeshBuiltinInputOp>(primOrVertexIndex)) {
      auto builtin = getMeshBuiltInInputOp->getBuiltin();
      if (builtin == BuiltInLocalInvocationIndex || builtin == BuiltInLocalInvocationId) {
        // Use LocalInvocationIndex or LocalInvocationId
        return true;
      }
    } else if (auto extractElement = dyn_cast<ExtractElementInst>(primOrVertexIndex)) {
      if (linearDispatch) {
        // Linear dispatch (X, Y=1, Z=1)
        auto vectorOp = extractElement->getVectorOperand();
        auto constIndexOp = dyn_cast<ConstantInt>(extractElement->getIndexOperand());
        if (fromLocalInvocationIndex(vectorOp) && constIndexOp && constIndexOp->getZExtValue() == 0) {
          // Use LocalInvocationID.X (equivalent to LocalInvocationIndex in linear dispatch
          return true;
        }
      }
    } else if (auto freeze = dyn_cast<FreezeInst>(primOrVertexIndex)) {
      return fromLocalInvocationIndex(freeze->getOperand(0));
    }

    return false;
  };

  IRBuilder<> builder(pipelineState->getContext());
  bool toAllocas = true;

  struct Payload {
    IRBuilder<> &builder;
    std::function<bool(Value *)> fromLocalInvocationIndex;
    bool &toAllocas;
  };
  Payload payload = {builder, fromLocalInvocationIndex, toAllocas};

  static const auto visitor =
      llvm_dialects::VisitorBuilder<Payload>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<SetMeshPrimitiveIndicesOp>([](Payload &payload, SetMeshPrimitiveIndicesOp &setMeshPrimitiveIndicesOp) {
            auto primitiveIndex = setMeshPrimitiveIndicesOp.getPrimitiveIndex();
            if (!payload.fromLocalInvocationIndex(primitiveIndex))
              payload.toAllocas = false;
          })
          .add<SetMeshPrimitiveCulledOp>([](Payload &payload, SetMeshPrimitiveCulledOp &setMeshPrimitiveCulledOp) {
            auto primitiveIndex = setMeshPrimitiveCulledOp.getPrimitiveIndex();
            if (!payload.fromLocalInvocationIndex(primitiveIndex))
              payload.toAllocas = false;
          })
          .add<WriteMeshOutputOp>([](Payload &payload, WriteMeshOutputOp &writeMeshOutputOp) {
            auto locationOffset = writeMeshOutputOp.getLocationOffset();
            auto primOrVertexIndex = writeMeshOutputOp.getPrimOrVertexIndex();
            if (locationOffset != payload.builder.getInt32(0))
              payload.toAllocas = false; // Output array indexing
            else if (!payload.fromLocalInvocationIndex(primOrVertexIndex))
              payload.toAllocas = false;
          })
          .build();
  visitor.visit(payload, *entryPoint);

  return toAllocas;
}

// =====================================================================================================================
// Process task shader lowering.
//
// @param entryPoint : Entry-point of task shader
void MeshTaskShader::processTaskShader(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStage::Task);

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
                            .add<GroupMemcpyOp>(&MeshTaskShader::lowerGroupMemcpy)
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
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

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
  //           - WriteMeshOutput -> Write output data to LDS
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
  //   if (vertexCount == -1) {
  //     if (threadIdInSubgroup == 0)
  //       Send message GS_ALLOC_REQ (vertexCount = 0, primitiveCount = 0)
  //     return
  //   }
  //
  //   if (vertexCount == 0)
  //     return
  //
  //   if (primitiveIndex < primitiveCount) {
  //     Read primitive connectivity data from LDS
  //     Read primitive built-ins from LDS
  //     Export primitive
  //   }
  //
  //   if (vertexIndex < vertexCount) {
  //     Read vertex built-ins from LDS
  //     Export vertex position data
  //   }
  //
  //   if (vertexIndex < vertexCount) {
  //     Read vertex attributes from LDS
  //     Export vertex attributes
  //   }
  //
  //   if (primitiveIndex < primitiveCount) {
  //     Read primitive attributes from LDS
  //     Export primitive attributes
  //   }
  //
  //   if (threadIdInSubgroup == 0)
  //     Write data to mesh pipeline statistics buffer
  //
  //   return
  // }
  //

  auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;

  // NOTE: We have to reset these two members since they might have stale values left by task shader processing.
  m_shaderRingEntryIndex = nullptr;
  m_payloadRingEntryOffset = nullptr;

  // Determine if barrier completion flag is needed
  m_needBarrierFlag = checkNeedBarrierFlag(entryPoint);

  auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Mesh);
  const bool rowExport = usesRowExport(m_pipelineState);

  // Setup LDS layout. We might shrink allocated LDS size if mesh outputs could be stored to allocas without LDS.
  const unsigned ldsSizeDwords = layoutMeshShaderLds(m_pipelineState, entryPoint, &m_ldsLayout, &m_outputsLayout);

  // Make sure we don't allocate more than what can legally be allocated by a single subgroup on the hardware.
  assert(ldsSizeDwords <= m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipMaxLdsSize);
  hwConfig.gsOnChipLdsSize = ldsSizeDwords;

  m_lds = getOrCreateMeshLds(entryPoint->getParent());

  // Mutate mesh shader entry-point
  entryPoint = mutateMeshShaderEntryPoint(entryPoint);

  // Force s_barrier to be present if necessary (ignore optimization)
  const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  // If we enable row export, the actual thread group size is determined by work group size provided from API mesh
  // shader.
  const unsigned flatWorkgroupSize = alignTo(rowExport ? numMeshThreads : hwConfig.primAmpFactor, waveSize);
  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        std::to_string(flatWorkgroupSize) + std::string(",") + std::to_string(flatWorkgroupSize));

  const unsigned numWaves = flatWorkgroupSize / waveSize;
  const unsigned numMeshWaves = alignTo(numMeshThreads, waveSize) / waveSize;

  const bool waAtmPrecedesPos =
      m_gfxIp.major >= 11 ? m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos : false;

  const unsigned numVertexAttributes = m_outputsLayout.vertexExportCount;
  const unsigned numPrimitiveAttributes = m_outputsLayout.primitiveExportCount;

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

  auto checkNoExportBlock = createBlock(".checkNoExport");
  auto skipExportBlock = createBlock(".skipExport");

  auto exportPrimitiveHeaderBlock = createBlock(".exportPrimitiveHeader");
  auto exportPrimitiveBodyBlock = createBlock(".exportPrimitiveBody");
  auto endExportPrimitiveBlock = createBlock(".endExportPrimitive");

  auto exportPositionHeaderBlock = createBlock(".exportPositionHeader");
  auto exportPositionBodyBlock = createBlock(".exportPositionBody");
  auto endExportPositionBlock = createBlock(".endExportPosition");

  auto exportVertexAttributeHeaderBlock = createBlock(".exportVertexAttributeHeader");
  auto exportVertexAttributeBodyBlock = createBlock(".exportVertexAttributeBody");
  auto endExportVertexAttributeBlock = createBlock(".endExportVertexAttribute");

  auto exportPrimitiveAttributeHeaderBlock = createBlock(".exportPrimitiveAttributeHeader");
  auto exportPrimitiveAttributeBodyBlock = createBlock(".exportPrimitiveAttributeBody");
  auto endExportPrimitiveAttributeBlock = createBlock(".endExportPrimitiveAttribute");

  if (waAtmPrecedesPos) {
    // Move position export blocks after attribute export blocks if ATM-precedes-pos workaround is required.
    exportPositionHeaderBlock->moveAfter(endExportPrimitiveAttributeBlock);
    exportPositionBodyBlock->moveAfter(exportPositionHeaderBlock);
    endExportPositionBlock->moveAfter(exportPositionBodyBlock);
  }

  auto collectMeshStatsBlock = createBlock(".collectMeshStats");
  auto exitBlock = createBlock(".exit");

  // Construct ".entry" block
  Value *firstThreadInSubgroup = nullptr;
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

    if (m_gfxIp.major >= 11)
      prepareAttribRingAccess();

    if (m_outputsLayout.outputsToAllocas) {
      firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, writeSpecialValueBlock, endWriteSpecialValueBlock);
    } else {
      m_builder.CreateBr(initPrimitiveIndicesHeaderBlock);
    }
  }

  PHINode *loopIndexPhi = nullptr;
  if (m_outputsLayout.outputsToAllocas) {
    // NOTE: If we can store mesh outputs to allocas, there is no need of initializing primitive indices in LDS.

    // Mark ".initPrimitiveIndicesHeader" block as unused
    {
      m_builder.SetInsertPoint(initPrimitiveIndicesHeaderBlock);
      m_builder.CreateUnreachable();
    }

    // Mark ".initPrimitiveIndicesBody" block as unused
    {
      m_builder.SetInsertPoint(initPrimitiveIndicesBodyBlock);
      m_builder.CreateUnreachable();
    }

    // Mark ".endInitPrimitiveIndices" block as unused
    {
      m_builder.SetInsertPoint(endInitPrimitiveIndicesBlock);
      m_builder.CreateUnreachable();
    }
  } else {
    // Construct ".initPrimitiveIndicesHeader" block
    {
      m_builder.SetInsertPoint(initPrimitiveIndicesHeaderBlock);

      if (rowExport) {
        loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
        loopIndexPhi->addIncoming(m_builder.getInt32(0), entryBlock); // loopIndex = 0

        // primitiveIndex = threadIdInSubgroup + loopIndex * waveSize
        m_waveThreadInfo.primOrVertexIndex =
            m_builder.CreateAdd(m_waveThreadInfo.threadIdInSubgroup,
                                m_builder.CreateMul(loopIndexPhi, m_builder.getInt32(waveSize)), "primitiveIndex");
      }

      auto validPrimitive =
          m_builder.CreateICmpULT(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(meshMode.outputPrimitives));
      m_builder.CreateCondBr(validPrimitive, initPrimitiveIndicesBodyBlock, endInitPrimitiveIndicesBlock);
    }

    // Construct ".initPrimitiveIndicesBody" block
    {
      m_builder.SetInsertPoint(initPrimitiveIndicesBodyBlock);

      if (rowExport) {
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
      m_builder.CreateBr(rowExport ? initPrimitiveIndicesHeaderBlock : endInitPrimitiveIndicesBlock);
    }

    // Construct ".endInitPrimitiveIndices" block
    {
      m_builder.SetInsertPoint(endInitPrimitiveIndicesBlock);

      firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, writeSpecialValueBlock, endWriteSpecialValueBlock);
    }
  }

  // Construct ".writeSpecialValue" block
  {
    m_builder.SetInsertPoint(writeSpecialValueBlock);

    // NOTE: We write invalid value (~0) to vertex count as the sentinel. If API mesh shader executes
    // SetMeshOutputs, the value will be changed to a valid one. Otherwise, we know SetMeshOutputs is not be
    // executed and we must make a dummy sendmsg (GS_ALLOC_REQ) with zero vertex/primitive count.
    auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::MeshOutputCounts));
    writeValueToLds(m_builder.getInt32(InvalidValue), ldsOffset);

    // Write barrier completion flag to LDS if it is required. Otherwise, skip it.
    if (m_needBarrierFlag) {
      auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::BarrierCompletion));
      writeValueToLds(m_builder.getInt32(0), ldsOffset);
    }

    // Write flat workgroup ID to LDS if it is required. Otherwise, skip it.
    if (useFlatWorkgroupId(m_pipelineState)) {
      auto ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::FlatWorkgroupId));
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
      auto flatWorkgroupId = getFunctionArgument(entryPoint, entryArgIdxs.flatWorkgroupId);
      writeValueToLds(flatWorkgroupId, ldsOffset);
    }

    m_builder.CreateBr(endWriteSpecialValueBlock);
  }

  // Construct ".endWriteSpecialValue" block
  {
    m_builder.SetInsertPoint(endWriteSpecialValueBlock);

    // NOTE: This barrier is for initialization of primitive indices in LDS, writing barrier completion flag to LDS, or
    // writing flat workgroup ID to LDS. If all cases are not encountered, this barrier is not needed.
    if (!m_outputsLayout.outputsToAllocas || m_needBarrierFlag || useFlatWorkgroupId(m_pipelineState))
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

    Value *ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::MeshOutputCounts));
    auto meshOutputCounts = readValueFromLds(m_builder.getInt64Ty(), ldsOffset, sizeof(uint64_t));
    meshOutputCounts =
        m_builder.CreateBitCast(meshOutputCounts, FixedVectorType::get(m_builder.getInt32Ty(), 2), "meshOutputCounts");

    vertexCount = m_builder.CreateExtractElement(meshOutputCounts, static_cast<uint64_t>(0));
    vertexCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane,
                                            vertexCount); // Promoted to SGPR
    vertexCount->setName("vertexCount");

    primitiveCount = m_builder.CreateExtractElement(meshOutputCounts, 1);
    primitiveCount = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane,
                                               primitiveCount); // Promoted to SGPR
    primitiveCount->setName("primitiveCount");

    auto dummyAllocReq = m_builder.CreateICmpEQ(vertexCount, m_builder.getInt32(InvalidValue));
    m_builder.CreateCondBr(dummyAllocReq, checkDummyAllocReqBlock, checkNoExportBlock);
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

    // We still collect mesh shader statistics in this special case. This is a valid mesh shader usage when the
    // primitive/vertex count is not specified by SetMeshOutputs (both are treated as zeros).
    collectMeshStatsInfo(entryPoint, m_builder.getInt32(0));

    m_builder.CreateBr(endDummyAllocReqBlock);
  }

  // Construct ".endDummyAllocReq" block
  {
    m_builder.SetInsertPoint(endDummyAllocReqBlock);

    m_builder.CreateRetVoid();
  }

  // Construct ".checkNoExport" block
  {
    m_builder.SetInsertPoint(checkNoExportBlock);

    // NOTE: When vertex count is 0, primitive count is 0 as well according to the processing of SetMeshOutputs.
    // In such case, we can skip primitive/vertex export and do early return.
    auto noExport = m_builder.CreateICmpEQ(vertexCount, m_builder.getInt32(0));
    m_builder.CreateCondBr(noExport, skipExportBlock, exportPrimitiveHeaderBlock);
  }

  // Construct ".skipExport" block
  {
    m_builder.SetInsertPoint(skipExportBlock);

    m_builder.CreateRetVoid();
  }

  // Construct ".exportPrimitiveHeader" block
  {
    m_builder.SetInsertPoint(exportPrimitiveHeaderBlock);

    if (rowExport) {
      loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder.getInt32(0), checkNoExportBlock); // loopIndex = 0

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

    if (rowExport) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   primitiveIndex = threadIdInSubgroup
      //   rowInSubgroup = waveIdInSubgroup
      //
      //   while (primitiveIndex < primitiveCount) {
      //     Export primitive
      //     loopIndex += numWaves
      //     primitiveIndex += loopIndex * waveSize
      //     rowInSubgroup += loopIndex
      //   }
      //
      auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportPrimitiveBodyBlock);
    }

    exportPrimitive();
    m_builder.CreateBr(rowExport ? exportPrimitiveHeaderBlock : endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveBlock);

    m_builder.CreateBr(waAtmPrecedesPos ? exportVertexAttributeHeaderBlock : exportPositionHeaderBlock);
  }

  // Construct ".exportPositionHeader" block
  {
    m_builder.SetInsertPoint(exportPositionHeaderBlock);

    if (rowExport) {
      loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
      loopIndexPhi->addIncoming(m_builder.getInt32(0), waAtmPrecedesPos ? endExportPrimitiveAttributeBlock
                                                                        : endExportPrimitiveBlock); // loopIndex = 0

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
    m_builder.CreateCondBr(validVertex, exportPositionBodyBlock, endExportPositionBlock);
  }

  // Construct "exportPositionBody" block
  {
    m_builder.SetInsertPoint(exportPositionBodyBlock);

    if (rowExport) {
      //
      // Row export is something like this:
      //
      //   loopIndex = 0
      //   vertexIndex = threadIdInSubgroup
      //   rowInSubgroup = waveIdInSubgroup
      //
      //   while (vertexIndex < vertexCount) {
      //     Export positions
      //     loopIndex += numWaves
      //     vertexIndex += loopIndex * waveSize
      //     rowInSubgroup += loopIndex
      //   }
      //
      auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
      loopIndexPhi->addIncoming(loopIndex, exportPositionBodyBlock);
    }

    exportPositions();
    m_builder.CreateBr(rowExport ? exportPositionHeaderBlock : endExportPositionBlock);
  }

  // Construct ".endExportPosition" block
  {
    m_builder.SetInsertPoint(endExportPositionBlock);

    if (waAtmPrecedesPos) {
      auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, collectMeshStatsBlock, exitBlock);
    } else {
      m_builder.CreateBr(exportVertexAttributeHeaderBlock);
    }
  }

  // Construct ".exportVertexAttributeHeader" block
  {
    m_builder.SetInsertPoint(exportVertexAttributeHeaderBlock);

    if (numVertexAttributes > 0) {
      if (rowExport) {
        loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
        loopIndexPhi->addIncoming(m_builder.getInt32(0),
                                  waAtmPrecedesPos ? endExportPrimitiveBlock : endExportPositionBlock); // loopIndex = 0

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
      m_builder.CreateCondBr(validVertex, exportVertexAttributeBodyBlock, endExportVertexAttributeBlock);
    } else {
      // No vertex attributes to export
      m_builder.CreateBr(endExportVertexAttributeBlock);
    }
  }

  // Construct "exportVertexAttributeBody" block
  {
    m_builder.SetInsertPoint(exportVertexAttributeBodyBlock);

    if (numVertexAttributes > 0) {
      if (rowExport) {
        //
        // Row export is something like this:
        //
        //   loopIndex = 0
        //   vertexIndex = threadIdInSubgroup
        //   rowInSubgroup = waveIdInSubgroup
        //
        //   while (vertexIndex < vertexCount) {
        //     Export vertex attributes
        //     loopIndex += numWaves
        //     vertexIndex += loopIndex * waveSize
        //     rowInSubgroup += loopIndex
        //   }
        //
        auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
        loopIndexPhi->addIncoming(loopIndex, exportVertexAttributeBodyBlock);
      }

      exportVertexAttributes();
      m_builder.CreateBr(rowExport ? exportVertexAttributeHeaderBlock : endExportVertexAttributeBlock);
    } else {
      // No vertex attributes to export
      m_builder.CreateUnreachable();
    }
  }

  // Construct ".endExportVertexAttribute" block
  {
    m_builder.SetInsertPoint(endExportVertexAttributeBlock);

    m_builder.CreateBr(exportPrimitiveAttributeHeaderBlock);
  }

  // Construct ".exportPrimitiveAttributeHeader" block
  {
    m_builder.SetInsertPoint(exportPrimitiveAttributeHeaderBlock);

    if (numPrimitiveAttributes > 0) {
      if (rowExport) {
        loopIndexPhi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
        loopIndexPhi->addIncoming(m_builder.getInt32(0), endExportVertexAttributeBlock); // loopIndex = 0

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
      m_builder.CreateCondBr(validPrimitive, exportPrimitiveAttributeBodyBlock, endExportPrimitiveAttributeBlock);
    } else {
      // No primitive attributes to export
      m_builder.CreateBr(endExportPrimitiveAttributeBlock);
    }
  }

  // Construct "exportPrimitiveAttributeBody" block
  {
    m_builder.SetInsertPoint(exportPrimitiveAttributeBodyBlock);

    if (numPrimitiveAttributes > 0) {
      if (rowExport) {
        //
        // Row export is something like this:
        //
        //   loopIndex = 0
        //   primitiveIndex = threadIdInSubgroup
        //   rowInSubgroup = waveIdInSubgroup
        //
        //   while (primitiveIndex < primitiveCount) {
        //     Export primitive attributes
        //     loopIndex += numWaves
        //     primitiveIndex += loopIndex * waveSize
        //     rowInSubgroup += loopIndex
        //   }
        //
        auto loopIndex = m_builder.CreateAdd(loopIndexPhi, m_builder.getInt32(numWaves)); // loopIndex += numWaves
        loopIndexPhi->addIncoming(loopIndex, exportPrimitiveAttributeBodyBlock);
      }

      exportPrimitiveAttributes();
      m_builder.CreateBr(rowExport ? exportPrimitiveAttributeHeaderBlock : endExportPrimitiveAttributeBlock);
    } else {
      // No primitive attributes to export
      m_builder.CreateUnreachable();
    }
  }

  // Construct ".endExportPrimitiveAttribute" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveAttributeBlock);

    if (waAtmPrecedesPos) {
      if (numVertexAttributes > 0 || numPrimitiveAttributes > 0) {
        // Before the first position export, add s_wait_vscnt 0 to make sure the completion of all
        // attributes being written to the attribute ring buffer
        m_builder.CreateFence(AtomicOrdering::Release, m_builder.getContext().getOrInsertSyncScopeID("agent"));
      }
      m_builder.CreateBr(exportPositionHeaderBlock);
    } else {
      auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, collectMeshStatsBlock, exitBlock);
    }
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

  // Mesh shader processing is done. We can safely update its input/output usage with final results.
  updateMeshShaderInOutUsage();
}

// =====================================================================================================================
// Lower GroupMemcpyOp - copy memory using all threads in a workgroup.
//
// @param groupMemcpyOp : Call instruction to do group memory copy
void MeshTaskShader::lowerGroupMemcpy(GroupMemcpyOp &groupMemcpyOp) {
  Function *entryPoint = groupMemcpyOp.getFunction();
  auto stage = getShaderStage(entryPoint);
  m_builder.SetInsertPoint(&groupMemcpyOp);

  unsigned scopeSize = 0;
  Value *threadIndex = nullptr;

  auto scope = groupMemcpyOp.getScope();
  if (scope == MemcpyScopeWorkGroup) {
    unsigned workgroupSize[3] = {};
    auto shaderModes = m_pipelineState->getShaderModes();
    if (stage == ShaderStage::Task) {
      Module &module = *groupMemcpyOp.getModule();
      workgroupSize[0] = shaderModes->getComputeShaderMode(module).workgroupSizeX;
      workgroupSize[1] = shaderModes->getComputeShaderMode(module).workgroupSizeY;
      workgroupSize[2] = shaderModes->getComputeShaderMode(module).workgroupSizeZ;
    } else if (stage == ShaderStage::Mesh) {
      workgroupSize[0] = shaderModes->getMeshShaderMode().workgroupSizeX;
      workgroupSize[1] = shaderModes->getMeshShaderMode().workgroupSizeY;
      workgroupSize[2] = shaderModes->getMeshShaderMode().workgroupSizeZ;
    } else {
      llvm_unreachable("Invalid shade stage!");
    }

    scopeSize = workgroupSize[0] * workgroupSize[1] * workgroupSize[2];
    threadIndex = m_waveThreadInfo.threadIdInSubgroup;
  } else {
    llvm_unreachable("Unsupported scope!");
  }

  MutateEntryPoint::processGroupMemcpy(groupMemcpyOp, m_builder, threadIndex, scopeSize);

  m_callsToRemove.push_back(&groupMemcpyOp);
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
  auto taskPayloadPtr = m_builder.create<BufferDescToPtrOp>(payloadRingBufDesc, true);
  taskPayloadPtrOp.replaceAllUsesWith(taskPayloadPtr);

  if (getShaderStage(entryPoint) == ShaderStage::Task)
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
  assert(getShaderStage(entryPoint) == ShaderStage::Task); // Must be task shader

  auto groupCountX = emitMeshTasksOp.getGroupCountX();
  auto groupCountY = emitMeshTasksOp.getGroupCountY();
  auto groupCountZ = emitMeshTasksOp.getGroupCountZ();

  // Mark the flag of mesh linear dispatch from task when the group count Y and Z are both ones
  if (isa<ConstantInt>(groupCountY) && isa<ConstantInt>(groupCountZ)) {
    const unsigned constGroupCountY = cast<ConstantInt>(groupCountY)->getZExtValue();
    const unsigned constGroupCountZ = cast<ConstantInt>(groupCountZ)->getZExtValue();
    bool enableLinearDispatch = constGroupCountY == 1 && constGroupCountZ == 1;
    m_pipelineState->getShaderResourceUsage(ShaderStage::Task)->builtInUsage.task.meshLinearDispatch =
        enableLinearDispatch;
  }

  auto emitMeshTasksCall = m_builder.GetInsertPoint();

  auto checkEmitMeshTasksBlock = m_builder.GetInsertBlock();
  auto emitMeshTasksBlock = checkEmitMeshTasksBlock->splitBasicBlock(emitMeshTasksCall, ".emitMeshTasks");
  auto endEmitMeshTasksBlock = emitMeshTasksBlock->splitBasicBlock(emitMeshTasksCall, ".endEmitMeshTasks");

  SyncScope::ID agentScope = m_builder.getContext().getOrInsertSyncScopeID("agent"); // Device level

  // Modify ".checkEmitMeshTasks" block
  {
    m_builder.SetInsertPoint(checkEmitMeshTasksBlock->getTerminator());

    if (m_accessTaskPayload) {
      // Make sure the task payload read/write access is completed
      m_builder.CreateFence(AtomicOrdering::Release, agentScope);
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
                                AtomicOrdering::Monotonic, agentScope);
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

    CoherentFlag coherent = {};
    if (m_gfxIp.major == 12)
      coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_SYS;

    m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                              {groupCount, drawDataRingBufDesc, m_builder.getInt32(0), drawDataRingEntryOffset,
                               m_builder.getInt32(coherent.u32All)});

    // NOTE: Only the lowest 8 bits are for us to write.
    Value *readyBit = getDrawDataReadyBit(entryPoint);
    readyBit = m_builder.CreateZExt(readyBit, m_builder.getInt8Ty());

    m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                              {readyBit, drawDataRingBufDesc, m_builder.getInt32(3 * sizeof(unsigned)),
                               drawDataRingEntryOffset, m_builder.getInt32(coherent.u32All)});
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

  assert(getShaderStage(setMeshOutputsOp.getFunction()) == ShaderStage::Mesh);

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

    // Check if vertex count or primitive count is zero. If so, set both to zero in order to disable vertex/primitive
    // exporting.
    auto productOfCounts = m_builder.CreateMul(vertexCount, primitiveCount);
    productOfCounts =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, productOfCounts);
    auto hasZeroCount = m_builder.CreateICmpEQ(productOfCounts, m_builder.getInt32(0));
    vertexCount = m_builder.CreateSelect(hasZeroCount, m_builder.getInt32(0), vertexCount);
    primitiveCount = m_builder.CreateSelect(hasZeroCount, m_builder.getInt32(0), primitiveCount);

    Value *ldsOffset = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::MeshOutputCounts));
    Value *meshOutputCounts = PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2));
    meshOutputCounts = m_builder.CreateInsertElement(meshOutputCounts, vertexCount, static_cast<uint64_t>(0));
    meshOutputCounts = m_builder.CreateInsertElement(meshOutputCounts, primitiveCount, 1);
    meshOutputCounts = m_builder.CreateBitCast(meshOutputCounts, m_builder.getInt64Ty(), "meshOutputCounts");
    writeValueToLds(meshOutputCounts, ldsOffset, sizeof(uint64_t));

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

  assert(getShaderStage(setMeshPrimitiveIndicesOp.getFunction()) == ShaderStage::Mesh);

  auto primitiveIndex = setMeshPrimitiveIndicesOp.getPrimitiveIndex();
  auto primitiveIndices = setMeshPrimitiveIndicesOp.getPrimitiveIndices();

  //
  // HW requires the primitive connectivity data has the following bit layout:
  //
  // Pre-GFX12:
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
  //
  // GFX12:
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //   | Null Primitive | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
  //   | [31]           | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
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
      primitiveData = m_builder.CreateShl(vertex1, 9);
      primitiveData = m_builder.CreateOr(primitiveData, vertex0);
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
      primitiveData = m_builder.CreateShl(vertex2, 9);
      primitiveData = m_builder.CreateOr(primitiveData, vertex1);
      primitiveData = m_builder.CreateShl(primitiveData, 9);
      primitiveData = m_builder.CreateOr(primitiveData, vertex0);
    }
  }

  // NOTE: We first clear old primitive connectivity data and use atomic OR operation to set new data. This is because
  // the null primitive flag might be set via built-in CullPrimitive.
  static const unsigned ClearMask = (1u << 31);

  if (m_outputsLayout.outputsToAllocas) {
    if (!m_outputsLayout.primitiveDataAlloca) {
      // Create alloca if not existing
      IRBuilder<>::InsertPointGuard guard(m_builder);
      m_builder.SetInsertPointPastAllocas(setMeshPrimitiveIndicesOp.getFunction());
      m_outputsLayout.primitiveDataAlloca = m_builder.CreateAlloca(m_builder.getInt32Ty(), nullptr, "primitiveData");
      m_outputsLayout.primitiveDataAlloca->setAlignment(Align(4));
    }
    auto primitiveDataAlloca = m_outputsLayout.primitiveDataAlloca;

    Value *newPrimitiveData = m_builder.CreateLoad(m_builder.getInt32Ty(), primitiveDataAlloca);
    newPrimitiveData = m_builder.CreateAnd(newPrimitiveData, ClearMask);
    newPrimitiveData = m_builder.CreateOr(newPrimitiveData, primitiveData);
    m_builder.CreateAlignedStore(newPrimitiveData, primitiveDataAlloca, Align(4));
  } else {
    Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
    Value *ldsOffset = m_builder.CreateAdd(ldsStart, primitiveIndex);

    atomicOpWithLds(AtomicRMWInst::And, m_builder.getInt32(ClearMask), ldsOffset);
    atomicOpWithLds(AtomicRMWInst::Or, primitiveData, ldsOffset);
  }

  m_callsToRemove.push_back(&setMeshPrimitiveIndicesOp);
}

// =====================================================================================================================
// Lower get mesh built-in value. Return the value of mesh built-in input.
//
// @param getMeshBuiltinInputOp : Call instruction op to return the value of mesh built-in input
void MeshTaskShader::lowerGetMeshBuiltinInput(GetMeshBuiltinInputOp &getMeshBuiltinInputOp) {
  m_builder.SetInsertPoint(&getMeshBuiltinInputOp);

  auto entryPoint = getMeshBuiltinInputOp.getFunction();
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

  Value *input = PoisonValue::get(getMeshBuiltinInputOp.getType());
  auto builtin = getMeshBuiltinInputOp.getBuiltin();
  switch (builtin) {
  case BuiltInDrawIndex: {
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
    input = getFunctionArgument(entryPoint, entryArgIdxs.drawIndex);
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      if (m_pipelineState->getShaderOptions(ShaderStage::Mesh).viewIndexFromDeviceIndex) {
        input = m_builder.getInt32(m_pipelineState->getDeviceIndex());
      } else {
        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
        input = getFunctionArgument(entryPoint, entryArgIdxs.viewId);
      }
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
    // Insert a call that later on might get lowered to code to reconfigure the workgroup.
    auto &mode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    unsigned workgroupSizeX = mode.workgroupSizeX;
    unsigned workgroupSizeY = mode.workgroupSizeY;
    unsigned workgroupSizeZ = mode.workgroupSizeZ;
    SwizzleWorkgroupLayout layout = calculateWorkgroupLayout(m_pipelineState, ShaderStage::Mesh);
    input = getMeshLocalInvocationId();
    if ((layout.microLayout == WorkgroupLayout::Quads) || (layout.macroLayout == WorkgroupLayout::SexagintiQuads)) {
      input = reconfigWorkgroupLayout(input, m_pipelineState, ShaderStage::Mesh, layout.macroLayout, layout.microLayout,
                                      workgroupSizeX, workgroupSizeY, workgroupSizeZ, false, m_builder);
    }
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
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStage::Mesh);
    assert(subgroupSize > 0 && subgroupSize % 32 == 0);
    input = m_builder.CreateLShr(localInvocationIndex, m_builder.getInt32(Log2_32(subgroupSize)));
    break;
  }
  case BuiltInNumSubgroups: {
    // numSubgroups = numMeshThreads / subgroupSize
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(ShaderStage::Mesh);
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
// @param setMeshPrimitiveCulledOp : Call instruction op to set primitive culled state
void MeshTaskShader::lowerSetMeshPrimitiveCulled(SetMeshPrimitiveCulledOp &setMeshPrimitiveCulledOp) {
  m_builder.SetInsertPoint(&setMeshPrimitiveCulledOp);

  assert(getShaderStage(setMeshPrimitiveCulledOp.getFunction()) == ShaderStage::Mesh);

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

  // NOTE: We first clear null primitive flag and use atomic OR operation to set new flag. This is because the
  // primitive connectivity data might be set via built-in PrimitiveXXXIndices.
  static const unsigned ClearMask = ~(1u << 31);

  if (m_outputsLayout.outputsToAllocas) {
    if (!m_outputsLayout.primitiveDataAlloca) {
      // Create alloca if not existing
      IRBuilder<>::InsertPointGuard guard(m_builder);
      m_builder.SetInsertPointPastAllocas(setMeshPrimitiveCulledOp.getFunction());
      m_outputsLayout.primitiveDataAlloca = m_builder.CreateAlloca(m_builder.getInt32Ty(), nullptr, "primitiveData");
      m_outputsLayout.primitiveDataAlloca->setAlignment(Align(4));
    }
    auto primitiveDataAlloca = m_outputsLayout.primitiveDataAlloca;

    Value *newPrimitiveData = m_builder.CreateLoad(m_builder.getInt32Ty(), primitiveDataAlloca);
    newPrimitiveData = m_builder.CreateAnd(newPrimitiveData, ClearMask);
    newPrimitiveData = m_builder.CreateOr(newPrimitiveData, nullPrimitive);
    m_builder.CreateAlignedStore(newPrimitiveData, primitiveDataAlloca, Align(4));
  } else {
    Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
    Value *ldsOffset = m_builder.CreateAdd(ldsStart, primitiveIndex);

    atomicOpWithLds(AtomicRMWInst::And, m_builder.getInt32(ClearMask), ldsOffset);
    atomicOpWithLds(AtomicRMWInst::Or, nullPrimitive, ldsOffset);
  }

  m_callsToRemove.push_back(&setMeshPrimitiveCulledOp);
}

// =====================================================================================================================
// Lower write mesh vertex/primitive output. Write mesh shader vertex/primitive outputs to LDS.
//
// @param WriteMeshOutputOp : Call instruction op to write vertex/primitive output for mesh shader
void MeshTaskShader::lowerWriteMeshOutput(WriteMeshOutputOp &writeMeshOutputOp) {
  m_builder.SetInsertPoint(&writeMeshOutputOp);

  assert(getShaderStage(writeMeshOutputOp.getFunction()) == ShaderStage::Mesh);

  auto isPrimitive = writeMeshOutputOp.getIsPrimitive();
  auto location = writeMeshOutputOp.getLocation();
  auto locationOffset = writeMeshOutputOp.getLocationOffset();
  auto componentIndex = writeMeshOutputOp.getComponentIndex();
  auto primOrVertexIndex = writeMeshOutputOp.getPrimOrVertexIndex();
  auto outputValue = writeMeshOutputOp.getOutputValue();

  auto &outputComponents =
      isPrimitive
          ? m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.primitiveOutputComponents
          : m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.vertexOutputComponents;
  assert(outputComponents.count(location) > 0); // Must exist
  const unsigned numComponents = outputComponents[location].first;

  if (m_outputsLayout.outputsToAllocas) {
    assert(locationOffset == m_builder.getInt32(0)); // Must not be output array indexing

    auto outputAllocaTy = FixedVectorType::get(m_builder.getFloatTy(), numComponents);

    auto &outputAllocas = isPrimitive ? m_outputsLayout.primitiveOutputAllocas : m_outputsLayout.vertexOutputAllocas;
    if (outputAllocas.count(location) == 0) {
      // Create alloca if not existing
      IRBuilder<>::InsertPointGuard guard(m_builder);
      m_builder.SetInsertPointPastAllocas(writeMeshOutputOp.getFunction());

      outputAllocas[location] = m_builder.CreateAlloca(
          outputAllocaTy, nullptr, (isPrimitive ? "primitiveOutput" : "vertexOutput") + std::to_string(location));
      outputAllocas[location]->setAlignment(Align(4));
    }

    auto outputAlloca = getOutputAlloca(location, isPrimitive);

    const unsigned bitWidth = outputValue->getType()->getScalarSizeInBits();
    unsigned numElements = outputValue->getType()->getPrimitiveSizeInBits() / bitWidth;

    // Bitcase the output to 32-bit value
    if (bitWidth == 32 || bitWidth == 64) {
      numElements *= (bitWidth / 32);
      outputValue = m_builder.CreateBitCast(outputValue, FixedVectorType::get(m_builder.getFloatTy(), numElements));
    } else if (bitWidth == 8 || bitWidth == 16) {
      if (outputValue->getType()->isFPOrFPVectorTy()) {
        outputValue =
            m_builder.CreateBitCast(outputValue, FixedVectorType::get(m_builder.getIntNTy(bitWidth), numElements));
      }
      outputValue = m_builder.CreateZExt(outputValue, FixedVectorType::get(m_builder.getInt32Ty(), numElements));
      outputValue = m_builder.CreateBitCast(outputValue, FixedVectorType::get(m_builder.getFloatTy(), numElements));
    }
    assert(outputValue->getType()->getScalarSizeInBits() == 32); // Must be 32-bit now

    if (outputAllocaTy == outputValue->getType()) {
      // Store the whole output
      assert(componentIndex == m_builder.getInt32(0));
      m_builder.CreateAlignedStore(outputValue, outputAlloca, Align(4));
    } else {
      // Store part of the output
      Value *newOutputValue = m_builder.CreateAlignedLoad(outputAllocaTy, outputAlloca, Align(4));

      // Scalarize output value
      SmallVector<Value *> outputValues;
      for (unsigned i = 0; i < numElements; ++i)
        outputValues.push_back(m_builder.CreateExtractElement(outputValue, i));

      // Insert output elements
      for (unsigned i = 0; i < outputValues.size(); ++i) {
        Value *insertIndex = componentIndex == m_builder.getInt32(0)
                                 ? m_builder.getInt32(i)
                                 : m_builder.CreateAdd(componentIndex, m_builder.getInt32(i));
        newOutputValue = m_builder.CreateInsertElement(newOutputValue, outputValues[i], insertIndex);
      }

      m_builder.CreateAlignedStore(newOutputValue, outputAlloca, Align(4));
    }
  } else {
    // ldsOffset = ldsStart + primOrVertexIndex * primOrVertexStride +
    //             offsetInPrimOrVertex + locationIndex * numComponents + componentIndex
    Value *ldsStart = m_builder.getInt32(
        getMeshShaderLdsRegionStart(isPrimitive ? MeshLdsRegion::PrimitiveOutput : MeshLdsRegion::VertexOutput));
    const unsigned primOrVertexStride = isPrimitive ? m_outputsLayout.primitiveStride : m_outputsLayout.vertexStride;
    Value *primOrVertexOffset = m_builder.CreateMul(primOrVertexIndex, m_builder.getInt32(primOrVertexStride));

    Value *offsetInPrimOrVertex = m_builder.getInt32(getOutputOffsetInPrimOrVertex(location, isPrimitive));
    if (locationOffset != m_builder.getInt32(0)) {
      auto locationIndex = locationOffset;

      if (numComponents > 4) {
        // NOTE: Here we encounter 64-bit vec3/vec4 data types. Such types will occupy two consecutive locations and the
        // provided location offset must be divided by 2 to get real location index.
        locationIndex = m_builder.CreateLShr(locationOffset, 2);
      }

      offsetInPrimOrVertex = m_builder.CreateAdd(offsetInPrimOrVertex,
                                                 m_builder.CreateMul(locationIndex, m_builder.getInt32(numComponents)));
    }

    if (componentIndex != m_builder.getInt32(0))
      offsetInPrimOrVertex = m_builder.CreateAdd(offsetInPrimOrVertex, componentIndex);

    auto ldsOffset = ldsStart;
    ldsOffset = m_builder.CreateAdd(ldsOffset, primOrVertexOffset);
    ldsOffset = m_builder.CreateAdd(ldsOffset, offsetInPrimOrVertex);

    writeValueToLds(outputValue, ldsOffset);
  }

  m_callsToRemove.push_back(&writeMeshOutputOp);
}

// =====================================================================================================================
// Initialize the wave/thread info from the entry-point.
//
// @param entryPoint : Shader entry-point
void MeshTaskShader::initWaveThreadInfo(Function *entryPoint) {
  m_waveThreadInfo = {}; // Reset it

  if (getShaderStage(entryPoint) == ShaderStage::Task) {
    // Task shader
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Task)->entryArgIdxs.task;

    if (m_gfxIp.major >= 12) {
#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
      m_waveThreadInfo.waveIdInSubgroup = m_builder.CreateIntrinsic(Intrinsic::amdgcn_wave_id, {});
#else
      m_waveThreadInfo.waveIdInSubgroup = m_builder.CreateIntrinsic(Intrinsic::amdgcn_wave_id, {}, {});
#endif
    } else {
      // waveId = dispatchInfo[24:20]
      m_waveThreadInfo.waveIdInSubgroup =
          m_builder.CreateAnd(m_builder.CreateLShr(getFunctionArgument(entryPoint, entryArgIdxs.multiDispatchInfo), 20),
                              0x1F, "waveIdInSubgroup");
    }
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Task);

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
    assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder.getInt64(-1));

    // waveId = mergedWaveInfo[27:24]
    Value *mergedWaveInfo =
        getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
    m_waveThreadInfo.waveIdInSubgroup =
        m_builder.CreateAnd(m_builder.CreateLShr(mergedWaveInfo, 24), 0xF, "waveIdInSubgroup");

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Mesh);

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
    if (getShaderStage(entryPoint) == ShaderStage::Task) {
      // NOTE: The calculation of shader ring entry index should be done at the beginning of the entry block. And the
      // value could be reused in subsequent operations.
      IRBuilder<>::InsertPointGuard guard(m_builder);
      m_builder.SetInsertPointPastAllocas(entryPoint);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Task)->entryArgIdxs.task;

      Value *workgroupIds[3] = {};
      if (m_gfxIp.major <= 11) {
        auto workgroupId = getFunctionArgument(entryPoint, entryArgIdxs.workgroupId);
        workgroupIds[0] = m_builder.CreateExtractElement(workgroupId, static_cast<uint64_t>(0));
        workgroupIds[1] = m_builder.CreateExtractElement(workgroupId, 1);
        workgroupIds[2] = m_builder.CreateExtractElement(workgroupId, 2);
      } else {
        // NOTE: On GFX12+, we use the intrinsics to get workgroup ID X/Y/Z instead of getting them from entry-point
        // arguments. This is because the IDs are modeled by architected dispatch ID GPRs rather than normal SGPRs.
#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
        workgroupIds[0] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_x, {});
        workgroupIds[1] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_y, {});
        workgroupIds[2] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_z, {});
#else
        workgroupIds[0] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_x, {}, {});
        workgroupIds[1] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_y, {}, {});
        workgroupIds[2] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_z, {}, {});
#endif
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
      assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
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
  assert(getShaderStage(entryPoint) == ShaderStage::Task); // Must be task shader

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
  assert(getShaderStage(entryPoint) == ShaderStage::Task); // Must be task shader

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
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh); // Must be mesh shader

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
  auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
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
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);

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
                            .add<WriteMeshOutputOp>(&MeshTaskShader::lowerWriteMeshOutput)
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
// Export primitive (primitive connectivity data and primitive payload).
void MeshTaskShader::exportPrimitive() {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;

  Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(MeshLdsRegion::PrimitiveIndices));
  Value *ldsOffset = m_builder.CreateAdd(ldsStart, m_waveThreadInfo.primOrVertexIndex);

  // The first dword is primitive connectivity data
  Value *primitiveIndices = nullptr;
  if (m_outputsLayout.outputsToAllocas) {
    if (m_outputsLayout.primitiveDataAlloca) {
      primitiveIndices =
          m_builder.CreateAlignedLoad(m_builder.getInt32Ty(), m_outputsLayout.primitiveDataAlloca, Align(4));
    } else {
      // No primitive indices have been written
      primitiveIndices = PoisonValue::get(m_builder.getInt32Ty());
    }
  } else {
    primitiveIndices = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);
  }
  assert(primitiveIndices);
  primitiveIndices->setName("primitiveIndices");

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
  if (builtInUsage.primitiveId) {
    Value *primitiveId = readBackMeshBuiltInOutput(BuiltInPrimitiveId);
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
    layer = readBackMeshBuiltInOutput(BuiltInLayer);

  Value *viewportIndex = nullptr;
  if (builtInUsage.viewportIndex)
    viewportIndex = readBackMeshBuiltInOutput(BuiltInViewportIndex);

  const bool enableMultiView = m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable;
  if (enableMultiView) {
    auto entryPoint = m_builder.GetInsertBlock()->getParent();
    const auto entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
    auto viewId = getFunctionArgument(entryPoint, entryArgIdxs.viewId);

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
    auto primitiveShadingRate = readBackMeshBuiltInOutput(BuiltInPrimitiveShadingRate);
    auto hwShadingRateMaskAndShift = convertToHwShadingRate(primitiveShadingRate);

    hwShadingRateMaskAndShift = m_builder.CreateAnd(hwShadingRateMaskAndShift, 0xF);
    hwShadingRateMaskAndShift = m_builder.CreateShl(hwShadingRateMaskAndShift, 28);

    if (primitivePayload)
      primitivePayload = m_builder.CreateOr(primitivePayload, hwShadingRateMaskAndShift);
    else
      primitivePayload = hwShadingRateMaskAndShift;
  }

  if (primitivePayload)
    primitivePayload->setName("primitivePayload");

  doExport(ExportKind::Primitive, ExportInfo{0, {primitiveIndices, primitivePayload}});
}

// =====================================================================================================================
// Export vertex positions.
void MeshTaskShader::exportPositions() {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;

  SmallVector<ExportInfo, 8> positionExports;

  if (builtInUsage.position) {
    auto position = readBackMeshBuiltInOutput(BuiltInPosition);
    std::array<Value *, 4> positions = {
        m_builder.CreateExtractElement(position, static_cast<uint64_t>(0)), m_builder.CreateExtractElement(position, 1),
        m_builder.CreateExtractElement(position, 2), m_builder.CreateExtractElement(position, 3)};
    positionExports.push_back({0, positions});
  }

  if (builtInUsage.pointSize) {
    auto pointSize = readBackMeshBuiltInOutput(BuiltInPointSize);
    positionExports.push_back({1, pointSize});
  }

  SmallVector<Value *, 8> clipDistances;
  if (builtInUsage.clipDistance > 0) {
    auto clipDistance = readBackMeshBuiltInOutput(BuiltInClipDistance);
    for (unsigned i = 0; i < builtInUsage.clipDistance; ++i)
      clipDistances.push_back(m_builder.CreateExtractElement(clipDistance, i));
  }

  SmallVector<Value *, 8> cullDistances;
  if (builtInUsage.cullDistance > 0) {
    auto cullDistance = readBackMeshBuiltInOutput(BuiltInCullDistance);
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

    unsigned exportSlot = builtInUsage.pointSize ? 2 : 1;
    positionExports.push_back(
        {exportSlot, {clipCullDistances[0], clipCullDistances[1], clipCullDistances[2], clipCullDistances[3]}});

    if (clipCullDistances.size() > 4) {
      // Do the second exporting
      positionExports.push_back(
          {exportSlot + 1, {clipCullDistances[4], clipCullDistances[5], clipCullDistances[6], clipCullDistances[7]}});
    }
  }

  doExport(ExportKind::Position, positionExports);
}

// =====================================================================================================================
// Export primitive attributes
void MeshTaskShader::exportPrimitiveAttributes() {
  SmallVector<ExportInfo, 32> attributeExports;

  // Export primitive attributes (from generic outputs)
  auto &primitiveOutputComponents =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.primitiveOutputComponents;
  for (auto &primitiveOutput : primitiveOutputComponents) {
    const auto location = primitiveOutput.first;
    const auto &[numComponents, forBuiltIn] = primitiveOutput.second;
    assert(numComponents > 0);

    if (forBuiltIn != InvalidValue)
      continue; // Skip those special outputs mapped from primitive built-ins. They will be handled later on.

    auto exportValue = readBackMeshGenericOutput(location, true);

    SmallVector<Value *, 8> exporteValues;
    for (unsigned i = 0; i < numComponents; ++i)
      exporteValues.push_back(m_builder.CreateExtractElement(exportValue, i));

    // Do array padding
    if (numComponents <= 4) {
      while (exporteValues.size() < 4) // <4 x float>
        exporteValues.push_back(nullptr);
    } else {
      while (exporteValues.size() < 8) // <8 x float>
        exporteValues.push_back(nullptr);
    }

    unsigned exportSlot = getOutputExportSlot(location, true);
    assert(exportSlot != InvalidValue);
    attributeExports.push_back({exportSlot, exporteValues[0], exporteValues[1], exporteValues[2], exporteValues[3]});
    if (numComponents > 4)
      attributeExports.push_back(
          {exportSlot + 1, exporteValues[4], exporteValues[5], exporteValues[6], exporteValues[7]});
  }

  // Export primitive attributes (from built-ins as generic ones)
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;

  if (builtInUsage.primitiveId) {
    const unsigned exportSlot = getOutputExportSlot(BuiltInPrimitiveId, true);
    if (exportSlot != InvalidValue) {
      Value *primitiveId = readBackMeshBuiltInOutput(BuiltInPrimitiveId);
      attributeExports.push_back({exportSlot, primitiveId});
    }
  }

  Value *layer = nullptr;
  bool exportLayer = false;
  if (builtInUsage.layer) {
    layer = readBackMeshBuiltInOutput(BuiltInLayer);
    exportLayer = true;
  } else {
    const auto nextStage = m_pipelineState->getNextShaderStage(ShaderStage::Mesh);
    if (nextStage == ShaderStage::Fragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;
      if (fsBuiltInUsage.layer) {
        // NOTE: In such case, mesh shader doesn't export layer while fragment shader expects to read it. We
        // export 0 to fragment shader, which is required by the spec.
        layer = m_builder.getInt32(0);
        exportLayer = true;
      }
    }
  }

  if (exportLayer) {
    const unsigned exportSlot = getOutputExportSlot(BuiltInLayer, true);
    if (exportSlot != InvalidValue) {
      assert(layer);
      attributeExports.push_back({exportSlot, layer});
    }
  }

  Value *viewportIndex = nullptr;
  bool exportViewportIndex = false;
  if (builtInUsage.viewportIndex) {
    viewportIndex = readBackMeshBuiltInOutput(BuiltInViewportIndex);
    exportViewportIndex = true;
  } else {
    const auto nextStage = m_pipelineState->getNextShaderStage(ShaderStage::Mesh);
    if (nextStage == ShaderStage::Fragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;
      if (fsBuiltInUsage.viewportIndex) {
        // NOTE: In such case, mesh shader doesn't export viewport index while fragment shader expects to read it. We
        // export 0 to fragment shader, which is required by spec.
        viewportIndex = m_builder.getInt32(0);
        exportViewportIndex = true;
      }
    }
  }

  if (exportViewportIndex) {
    const unsigned exportSlot = getOutputExportSlot(BuiltInViewportIndex, true);
    if (exportSlot != InvalidValue) {
      assert(viewportIndex);
      attributeExports.push_back({exportSlot, viewportIndex});
    }
  }

  doExport(ExportKind::PrimitiveAttribute, attributeExports);
}

// =====================================================================================================================
// Export vertex attributes
void MeshTaskShader::exportVertexAttributes() {
  SmallVector<ExportInfo, 32> attributeExports;

  // Export vertex attributes (from generic outputs)
  auto &vertexOutputComponents =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.vertexOutputComponents;
  for (auto &vertexOutput : vertexOutputComponents) {
    const auto location = vertexOutput.first;
    const auto &[numComponents, forBuiltIn] = vertexOutput.second;
    assert(numComponents > 0);

    if (forBuiltIn != InvalidValue)
      continue; // Skip those special outputs mapped from vertex built-ins. They will be handled later on.

    auto exportValue = readBackMeshGenericOutput(location, false);

    SmallVector<Value *, 8> exporteValues;
    for (unsigned i = 0; i < numComponents; ++i)
      exporteValues.push_back(m_builder.CreateExtractElement(exportValue, i));

    // Do array padding
    if (numComponents <= 4) {
      while (exporteValues.size() < 4) // <4 x float>
        exporteValues.push_back(nullptr);
    } else {
      while (exporteValues.size() < 8) // <8 x float>
        exporteValues.push_back(nullptr);
    }

    unsigned exportSlot = getOutputExportSlot(location, false);
    assert(exportSlot != InvalidValue);
    attributeExports.push_back({exportSlot, exporteValues[0], exporteValues[1], exporteValues[2], exporteValues[3]});
    if (numComponents > 4)
      attributeExports.push_back(
          {exportSlot + 1, exporteValues[4], exporteValues[5], exporteValues[6], exporteValues[7]});
  }

  // Export vertex attributes (from built-ins as generic ones)
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;

  if (builtInUsage.clipDistance > 0 || builtInUsage.cullDistance > 0) {
    assert(builtInUsage.clipDistance + builtInUsage.cullDistance <= MaxClipCullDistanceCount);

    SmallVector<Value *, 8> clipDistances;
    if (builtInUsage.clipDistance > 0) {
      auto clipDistance = readBackMeshBuiltInOutput(BuiltInClipDistance);
      for (unsigned i = 0; i < builtInUsage.clipDistance; ++i)
        clipDistances.push_back(m_builder.CreateExtractElement(clipDistance, i));
    }

    SmallVector<Value *, 8> cullDistances;
    if (builtInUsage.cullDistance > 0) {
      auto cullDistance = readBackMeshBuiltInOutput(BuiltInCullDistance);
      for (unsigned i = 0; i < builtInUsage.cullDistance; ++i)
        cullDistances.push_back(m_builder.CreateExtractElement(cullDistance, i));
    }

    // Merge clipDistance and cullDistance
    SmallVector<Value *, 8> clipCullDistances;
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

    bool exportClipCullDistance = true;

    auto nextStage = m_pipelineState->getNextShaderStage(ShaderStage::Mesh);
    if (nextStage == ShaderStage::Fragment) {
      const auto &fsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;

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
      unsigned exportSlot = getOutputExportSlot(BuiltInClipDistance, false);
      if (exportSlot == InvalidValue) {
        // If ClipDistance doesn't exist, check CullDistance once again
        exportSlot = getOutputExportSlot(BuiltInCullDistance, false);
      }
      assert(exportSlot != InvalidValue);

      attributeExports.push_back(
          {exportSlot, {clipCullDistances[0], clipCullDistances[1], clipCullDistances[2], clipCullDistances[3]}});

      if (clipCullDistances.size() > 4) {
        // Do the second exporting
        attributeExports.push_back(
            {exportSlot + 1, {clipCullDistances[4], clipCullDistances[5], clipCullDistances[6], clipCullDistances[7]}});
      }
    }
  }

  doExport(ExportKind::VertexAttribute, attributeExports);
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
  SyncScope::ID agentScope = m_builder.getContext().getOrInsertSyncScopeID("agent"); // Device level

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
                              AtomicOrdering::Monotonic, agentScope);
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
                              AtomicOrdering::Monotonic, agentScope);
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
    case ExportKind::Position:
      target = EXP_TARGET_POS_0;
      break;
    case ExportKind::Primitive:
      target = EXP_TARGET_PRIM;
      break;
    case ExportKind::VertexAttribute:
    case ExportKind::PrimitiveAttribute:
      target = EXP_TARGET_PARAM_0;
      break;
    default:
      llvm_unreachable("Unexpected export target!");
      break;
    }

    bool exportDone = false;
    if ((kind == ExportKind::Position || kind == ExportKind::Primitive) && i == exports.size() - 1)
      exportDone = true; // Last export

    if (m_gfxIp.major >= 11) {
      if (m_pipelineState->attributeThroughExport() || kind == ExportKind::Position || kind == ExportKind::Primitive) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp_row, valueTy,
                                  {
                                      m_builder.getInt32(target + exports[i].slot), // tgt
                                      m_builder.getInt32(validMask),                // en
                                      values[0],                                    // src0
                                      values[1] ? values[1] : poison,               // src1
                                      values[2] ? values[2] : poison,               // src2
                                      values[3] ? values[3] : poison,               // src3
                                      m_builder.getInt1(exportDone),                // done
                                      m_waveThreadInfo.rowInSubgroup,               // row number
                                  });
      } else {
        assert(kind == ExportKind::VertexAttribute || kind == ExportKind::PrimitiveAttribute);
        assert(!m_pipelineState->attributeThroughExport());

        Value *valueToStore = PoisonValue::get(FixedVectorType::get(valueTy, 4));
        for (unsigned j = 0; j < 4; ++j) {
          if (values[j])
            valueToStore = m_builder.CreateInsertElement(valueToStore, values[j], j);
        }

        // ringOffset = attribRingBaseOffset + 32 * exportSlot * 16
        //            = attribRingBaseOffset + exportSlot * 512
        auto locationOffset = m_builder.getInt32(exports[i].slot * SizeOfVec4);

        CoherentFlag coherent = {};
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
          coherent.bits.glc = true;
        } else {
          coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
        }

        m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_struct_buffer_store,
                                  {valueToStore, m_attribRingBufDesc, m_waveThreadInfo.primOrVertexIndex,
                                   locationOffset, m_attribRingBaseOffset, m_builder.getInt32(coherent.u32All)});
      }
    } else {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, valueTy,
                                {
                                    m_builder.getInt32(target + exports[i].slot), // tgt
                                    m_builder.getInt32(validMask),                // en
                                    values[0],                                    // src0
                                    values[1] ? values[1] : poison,               // src1
                                    values[2] ? values[2] : poison,               // src2
                                    values[3] ? values[3] : poison,               // src3
                                    m_builder.getInt1(exportDone),                // done
                                    m_builder.getFalse(),                         // vm
                                });
    }
  }
}

// =====================================================================================================================
// Prepare attribute ring access by collecting attribute count, modifying the STRIDE field of attribute ring buffer
// descriptor, and calculating subgroup's attribute ring base offset.
void MeshTaskShader::prepareAttribRingAccess() {
  assert(m_gfxIp.major >= 11); // Must be GFX11+

  unsigned numAttributes = m_outputsLayout.vertexExportCount + m_outputsLayout.primitiveExportCount;
  if (numAttributes == 0)
    return; // No attribute export

  // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and PRIM_EXPORT_COUNT.
  // When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even though this is not what we
  // want. Hence, we should reserve param0 as a dummy vertex attribute.
  if (m_outputsLayout.vertexExportCount == 0)
    ++numAttributes; // Count in this dummy vertex attribute

  // attribRingBase[14:0]
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  Value *attribRingBase =
      getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
  attribRingBase = m_builder.CreateAnd(attribRingBase, 0x7FFF);

  m_attribRingBaseOffset =
      m_builder.CreateMul(attribRingBase, m_builder.getInt32(AttributeGranularity), "attribRingBaseOffset");

  m_attribRingBufDesc = m_pipelineSysValues.get(entryPoint)->getAttribRingBufDesc();

  // Modify the field STRIDE of attribute ring buffer descriptor
  if (numAttributes >= 2) {
    // NOTE: STRIDE is initialized to 16 by the driver, which is the right value for one attribute.
    // We have to override the value if there are more attributes.
    auto stride = m_builder.getInt32(numAttributes * SizeOfVec4);
    setBufferStride(m_gfxIp, m_builder, m_attribRingBufDesc, stride);
  }
}

// =====================================================================================================================
// Get the flat workgroup ID of mesh shader.
//
// @returns : Value of flat workgroup ID
Value *MeshTaskShader::getMeshFlatWorkgroupId() {
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStage::Mesh); // Must be mesh shader

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
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh); // Must be mesh shader

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;
  return getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);
}

// =====================================================================================================================
// Get the built-in WorkgroupId of mesh shader.
//
// @returns : Value of the built-in WorkgroupId
Value *MeshTaskShader::getMeshWorkgroupId() {
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh); // Must be mesh shader

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

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;

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
  assert(getShaderStage(entryPoint) == ShaderStage::Mesh); // Must be mesh shader

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
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Mesh)->entryArgIdxs.mesh;

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
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStage::Mesh); // Must be mesh shader
  return m_waveThreadInfo.threadIdInSubgroup;
}

// =====================================================================================================================
// Get the built-in GlobalInvocationId of mesh shader.
//
// @returns : Value of the built-in GlobalInvocationId
Value *MeshTaskShader::getMeshGlobalInvocationId() {
  assert(getShaderStage(m_builder.GetInsertBlock()->getParent()) == ShaderStage::Mesh); // Must be mesh shader

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
// Read back mesh shader built-in output value from output allocas or LDS, which is supposed to be written by mesh
// shader execution.
//
// @param builtIn : Mesh shader built-in
// @returns : The built-in output value from output allocas or LDS
Value *MeshTaskShader::readBackMeshBuiltInOutput(BuiltInKind builtIn) {
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage;

  bool primitive = (builtIn == BuiltInPrimitiveId || builtIn == BuiltInViewportIndex || builtIn == BuiltInLayer ||
                    builtIn == BuiltInPrimitiveShadingRate);

  unsigned location = InvalidValue;
  MeshLdsRegion region = MeshLdsRegion::VertexOutput;

  if (primitive) {
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

  Value *readValue = nullptr;

  if (m_outputsLayout.outputsToAllocas) {
    auto outputAlloca = getOutputAlloca(location, primitive);
    readValue = m_builder.CreateAlignedLoad(readTy, outputAlloca, Align(4));
  } else {
    // ldsOffset = ldsStart + primOrVertexIndex * primOrVertexStride + offsetInPrimOrVertex
    Value *primOrVertexOffset = nullptr;
    if (region == MeshLdsRegion::VertexOutput) {
      primOrVertexOffset =
          m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(m_outputsLayout.vertexStride));
    } else {
      assert(region == MeshLdsRegion::PrimitiveOutput);
      primOrVertexOffset =
          m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(m_outputsLayout.primitiveStride));
    }

    Value *ldsStart = m_builder.getInt32(getMeshShaderLdsRegionStart(
        region == MeshLdsRegion::PrimitiveOutput ? MeshLdsRegion::PrimitiveOutput : MeshLdsRegion::VertexOutput));
    Value *offsetInPrimOrVertex =
        m_builder.getInt32(getOutputOffsetInPrimOrVertex(location, region == MeshLdsRegion::PrimitiveOutput));

    auto ldsOffset = ldsStart;
    ldsOffset = m_builder.CreateAdd(ldsOffset, primOrVertexOffset);
    ldsOffset = m_builder.CreateAdd(ldsOffset, offsetInPrimOrVertex);

    readValue = readValueFromLds(readTy, ldsOffset);
  }

  return readValue;
}

// =====================================================================================================================
// Read back mesh shader generic output value from output allocas or LDS, which is supposed to be written by mesh
// shader execution.
//
// @param location : Output generic location
// @param primitive : Whether this is a primitive output
// @returns : The generic output value from output allocas or LDS
Value *MeshTaskShader::readBackMeshGenericOutput(unsigned location, bool primitive) {
  auto &outputComponents =
      primitive ? m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.primitiveOutputComponents
                : m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage.mesh.vertexOutputComponents;
  assert(outputComponents.count(location) > 0); // Must exist
  const unsigned numComponents = outputComponents[location].first;

  Value *readValue = nullptr;
  auto readTy = FixedVectorType::get(m_builder.getFloatTy(), numComponents);

  if (m_outputsLayout.outputsToAllocas) {
    auto outputAlloca = getOutputAlloca(location, primitive);
    readValue = m_builder.CreateAlignedLoad(readTy, outputAlloca, Align(4));
  } else {
    Value *ldsStart = m_builder.getInt32(
        getMeshShaderLdsRegionStart(primitive ? MeshLdsRegion::PrimitiveOutput : MeshLdsRegion::VertexOutput));
    const unsigned primOrVertexStride = primitive ? m_outputsLayout.primitiveStride : m_outputsLayout.vertexStride;
    Value *primOrVertexOffset =
        m_builder.CreateMul(m_waveThreadInfo.primOrVertexIndex, m_builder.getInt32(primOrVertexStride));

    auto offsetInPrimOrVertex = m_builder.getInt32(getOutputOffsetInPrimOrVertex(location, primitive));

    auto ldsOffset = ldsStart;
    ldsOffset = m_builder.CreateAdd(ldsOffset, primOrVertexOffset);
    ldsOffset = m_builder.CreateAdd(ldsOffset, offsetInPrimOrVertex);

    readValue = readValueFromLds(readTy, ldsOffset);
  }

  return readValue;
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
// Update input/output usage in resource usage for mesh shader. The info will be used to build register metadata later
// on.
void MeshTaskShader::updateMeshShaderInOutUsage() {
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->inOutUsage;

  inOutUsage.expCount = m_outputsLayout.vertexExportCount;
  inOutUsage.primExpCount = m_outputsLayout.primitiveExportCount;

  // For part pipeline, below info will be used to build the metadata ".preraster_output_semantic" to correctly map
  // output semantic locations specified by API mesh shader to HW export slots. The export slots will be used to fill
  // the register field SPI_PS_INPUT_CNTL.OFFSET during pipeline linking.
  if (m_pipelineState->isUnlinked()) {
    for (auto it = inOutUsage.outputLocInfoMap.begin(); it != inOutUsage.outputLocInfoMap.end();) {
      // Revisit each entry of vertex outputs. If it is recorded and processed by mesh shader, update the mapping
      // location to HW export slot. Otherwise, remove this entry.
      const unsigned mappingLocation = it->second.getLocation();
      if (m_outputsLayout.vertexGenericExports.count(mappingLocation) > 0) {
        const unsigned exportSlot = m_outputsLayout.vertexGenericExports[mappingLocation];
        it->second.setLocation(exportSlot);
        it++;
      } else {
        inOutUsage.outputLocInfoMap.erase(it++);
      }
    }

    inOutUsage.builtInOutputLocMap.clear();
    for (auto &builtInExport : m_outputsLayout.vertexBuiltInExports) {
      const auto &[builtIn, exportSlot] = builtInExport;
      inOutUsage.builtInOutputLocMap[builtIn] = exportSlot;
    }

    for (auto it = inOutUsage.perPrimitiveOutputLocMap.begin(); it != inOutUsage.perPrimitiveOutputLocMap.end();) {
      // Revisit each entry of primitive outputs. If it is recorded and processed by mesh shader, update the mapping
      // location to HW export slot. Otherwise, remove this entry.
      const unsigned mappingLocation = it->second;
      if (m_outputsLayout.primitiveGenericExports.count(mappingLocation) > 0) {
        const unsigned exportSlot = m_outputsLayout.primitiveGenericExports[mappingLocation];
        it->second = exportSlot;
        it++;
      } else {
        inOutUsage.perPrimitiveOutputLocMap.erase(it++);
      }
    }

    inOutUsage.perPrimitiveBuiltInOutputLocMap.clear();
    for (auto &builtInExport : m_outputsLayout.primitiveBuiltInExports) {
      const auto &[builtIn, exportSlot] = builtInExport;
      inOutUsage.perPrimitiveBuiltInOutputLocMap[builtIn] = exportSlot;
    }
  }
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
  if (usesRowExport(m_pipelineState))
    return false; // Not needed if row export is enable

  const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  const unsigned numThreads =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.primAmpFactor;
  assert(numThreads >= numMeshThreads);

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Mesh);
  const unsigned numMeshWaves = alignTo(numMeshThreads, waveSize) / waveSize;
  const unsigned numWaves = alignTo(numThreads, waveSize) / waveSize;
  if (numWaves == numMeshWaves)
    return false; // Wave number to run API mesh shader is equal to actual wave number to run HW mesh shader (HW GS)

  assert(getShaderStage(entryPoint) == ShaderStage::Mesh);
  auto module = entryPoint->getParent();
  for (auto &func : module->functions()) {
    if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_s_barrier ||
                               func.getIntrinsicID() == Intrinsic::amdgcn_s_barrier_signal)) {
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
// @param alignment : Alignment of read operation (in bytes)
// @returns : The Value read from LDS
Value *MeshTaskShader::readValueFromLds(Type *readTy, Value *ldsOffset, unsigned alignment) {
  assert(m_lds);
  assert(readTy->isIntOrIntVectorTy() || readTy->isFPOrFPVectorTy());

  Value *readPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);

  const unsigned bitWidth = readTy->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // For 8-bit and 16-bit data type, we read them as 32-bit types from LDS. They are not packed tightly in LDS.
    unsigned numElems = readTy->isVectorTy() ? cast<FixedVectorType>(readTy)->getNumElements() : 1;

    Type *newReadTy = m_builder.getInt32Ty();
    if (readTy->isVectorTy())
      newReadTy = FixedVectorType::get(m_builder.getInt32Ty(), numElems);

    readPtr =
        m_builder.CreateBitCast(readPtr, PointerType::get(newReadTy, readPtr->getType()->getPointerAddressSpace()));
    Value *readValue = m_builder.CreateAlignedLoad(newReadTy, readPtr, Align(alignment));

    Type *truncTy = m_builder.getIntNTy(bitWidth);
    if (readTy->isVectorTy())
      truncTy = FixedVectorType::get(m_builder.getIntNTy(bitWidth), numElems);

    readValue = m_builder.CreateTrunc(readValue, truncTy);

    if (readTy->isFPOrFPVectorTy())
      readValue = m_builder.CreateBitCast(readValue, readTy);

    return readValue;
  }

  readPtr = m_builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
  return m_builder.CreateAlignedLoad(readTy, readPtr, Align(alignment));
}

// =====================================================================================================================
// Write value to mesh shader LDS.
//
// @param writeValue : Value to write
// @param ldsOffset : LDS offset in dwords
// @param alignment : Alignment of write operation (in bytes)
void MeshTaskShader::writeValueToLds(Value *writeValue, Value *ldsOffset, unsigned alignment) {
  assert(m_lds);

  auto writeTy = writeValue->getType();
  assert(writeTy->isIntOrIntVectorTy() || writeTy->isFPOrFPVectorTy());

  Value *writePtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);

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
    m_builder.CreateAlignedStore(writeValue, writePtr, Align(alignment));
    return;
  }

  writePtr = m_builder.CreateBitCast(
      writePtr, PointerType::get(writeValue->getType(), writePtr->getType()->getPointerAddressSpace()));
  m_builder.CreateAlignedStore(writeValue, writePtr, Align(alignment));
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
  Value *atomicPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);
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
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 12) {
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_signal, {}, m_builder.getInt32(WorkgroupNormalBarrierId));
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_wait, {},
                              m_builder.getInt16(static_cast<uint16_t>(WorkgroupNormalBarrierId)));
    return;
  }

#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {});
#else
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
#endif
}

} // namespace lgc
