/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayQuery.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvLowerRayQuery
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLowerRayTracingIntrinsics.h"

#pragma pack(push, 4)
// Acceleration structure result data offsets
struct ResultDataOffsets {
  unsigned internalNodes; // Offset to internal box nodes
  unsigned leafNodes;     // Offset to leaf nodes
  unsigned sideband;      // Offset to sideband data (BVH4 bottom level only)
  unsigned geometryInfo;  // Offset to geometry desc info (bottom level only)
  unsigned primNodePtrs;  // Offset to prim node pointers (BVH4 with triangle compression and ALLOW_UPDATE only)
};
#pragma pack(pop)

#pragma pack(push, 4)
// Header for acceleration structure
struct AccelStructHeader {
  unsigned type;                 // Type of acceleration structure (Top level or bottom level)
  unsigned metadataSizeInBytes;  // Total size of the metadata in bytes
  unsigned sizeInBytes;          // Total size of the structure in bytes (Including this header)
  unsigned numPrimitives;        // Number of primitives encoded in the structure
  unsigned numActivePrims;       // Tracks the number of active prims to add to bvh
  unsigned taskCounter;          // Used in update parallel path to synchronize thread groups
  unsigned numDescs;             // Number of instance/geometry descs in the structure
  unsigned geometryType;         // Type of geometry contained in the bottom level structure
  ResultDataOffsets dataOffsets; // Offsets within accel struct (not including the header)
  unsigned numInternalNodes;     // Number of internal nodes used by the acceleration structure after building
  unsigned numLeafNodes;         // Number of leaf nodes used by the acceleration structure after building
  unsigned bboxMin[3];           // 32bit bounding box (float3), min. Set only if root node is a box
  unsigned bboxMax[3];           // 32bit bounding box (float3), max. Set only if root node is a box
  unsigned padding[11];          // Padding bytes for 128-byte alignment (Gfx10 cacheline size)
};
#pragma pack(pop)

#pragma pack(push, 4)
// Header for ray tracing instance descriptor
struct RayTracingInstanceDesc {
  float Transform[3][4];                                  // Inverse transform for traversal
  uint32_t InstanceID_and_Mask;                           // 24-bit instance ID and 8-bit mask
  uint32_t InstanceContributionToHitGroupIndex_and_Flags; // 24-bit instance contribution and 8-bit flags
  uint32_t accelStructureAddressLo;                       // Lower part of acceleration structure base address
  uint32_t accelStructureAddressHiAndFlags;               // Upper part of acceleration structure base address and
};
#pragma pack(pop)

#pragma pack(push, 4)
// Header for ray tracing instance extra data
struct RayTracingInstanceExtraData {
  uint32_t instanceIndex;
  uint32_t blasNodePointer; // might not point to root
  uint32_t blasMetadataSize;
  uint32_t padding0;
  float Transform[3][4]; // Non-inverse
};
#pragma pack(pop)

#pragma pack(push, 4)
// Header for ray tracing instance node
struct RayTracingInstanceNode {
  RayTracingInstanceDesc desc;
  RayTracingInstanceExtraData extra;
};
#pragma pack(pop)

namespace Llpc {

// Corresponds to gl_RayFlags* in GLSL_EXT_ray_tracing.txt
enum RayFlag : unsigned {
  None = 0x0000,                       // gl_RayFlagsNoneEXT
  ForceOpaque = 0x0001,                // gl_RayFlagsOpaqueEXT
  ForceNonOpaque = 0x0002,             // gl_RayFlagsNoOpaqueEXT
  AcceptFirstHitAndEndSearch = 0x0004, // gl_RayFlagsTerminateOnFirstHitEXT
  SkipClosestHitShader = 0x0008,       // gl_RayFlagsSkipClosestHitShaderEXT
  CullBackFacingTriangles = 0x0010,    // gl_RayFlagsCullBackFacingTrianglesEXT
  CullFrontFacingTriangles = 0x0020,   // gl_RayFlagsCullFrontFacingTrianglesEXT
  CullOpaque = 0x0040,                 // gl_RayFlagsCullOpaqueEXT
  CullNonOpaque = 0x0080,              // gl_RayFlagsCullNoOpaqueEXT
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering ray query.
class SpirvLowerRayQuery : public SpirvLowerRayTracingIntrinsics {
public:
  SpirvLowerRayQuery();
  SpirvLowerRayQuery(bool rayQueryLibrary);
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  virtual bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "Lower SPIR-V RayQuery operations"; }

  const static unsigned MaxLdsStackEntries = 16;

protected:
  void processLibraryFunction(llvm::Function *&func);
  void processShaderFunction(llvm::Function *func, unsigned opcode);
  void createGlobalStack();
  void createGlobalLdsUsage();
  void createGlobalRayQueryObj();
  void createGlobalTraceRayStaticId();
  void initGlobalVariable();
  void generateTraceRayStaticId();
  llvm::Value *createTransformMatrix(unsigned builtInId, llvm::Value *accelStruct, llvm::Value *instanceId,
                                     llvm::Instruction *insertPos);
  llvm::Value *getThreadIdInGroup() const;
  void eraseFunctionBlocks(llvm::Function *func);
  unsigned getFuncOpcode(llvm::Function *func);
  llvm::Value *createLoadInstanceIndex(llvm::Value *instNodeAddr);
  llvm::Value *createLoadInstanceId(llvm::Value *instNodeAddr);
  llvm::Value *createLoadMatrixFromAddr(llvm::Value *matrixAddr);
  llvm::GlobalVariable *m_traceRayStaticId; // Static trace ray call site identifier

  bool m_rayQueryLibrary;       // Whether the module is ray query library
  unsigned m_spirvOpMetaKindId; // Metadata kind ID for "spirv.op"
private:
  template <spv::Op> void createRayQueryFunc(llvm::Function *func);
  void createRayQueryProceedFunc(llvm::Function *func);
  llvm::Value *createIntersectSystemValue(llvm::Function *func, unsigned raySystem);
  void createWriteLdsStack(llvm::Function *func);
  void createReadLdsStack(llvm::Function *func);
  void createIntersectMatrix(llvm::Function *func, unsigned builtInId);
  void createIntersectBvh(llvm::Function *func);
  void createSampleGpuTime(llvm::Function *func);
#if VKI_BUILD_GFX11
  void createLdsStackInit(llvm::Function *func);
  void createLdsStackStore(llvm::Function *func);
#endif
  llvm::Value *getStackArrayIndex(llvm::Value *stackOffset);
  uint32_t getWorkgroupSize() const;
  llvm::Value *createGetInstanceNodeAddr(llvm::Value *instNodePtr, llvm::Value *rayQuery);
  llvm::Value *getDispatchId();
  llvm::Value *createGetBvhSrd(llvm::Value *expansion, llvm::Value *boxSortMode);
  bool stageNotSupportLds(ShaderStage stage);
  llvm::GlobalVariable *m_ldsStack;        // LDS to hold stack value
  llvm::GlobalVariable *m_ldsUsage;        // LDS usage
  llvm::GlobalVariable *m_stackArray;      // Stack array to hold stack value
  llvm::GlobalVariable *m_prevRayQueryObj; // Previous ray query Object
  llvm::GlobalVariable *m_rayQueryObjGen;  // Ray query Object Id generator
  unsigned m_nextTraceRayId;               // Next trace ray ID to be used for ray history
};

} // namespace Llpc
