/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  VertexFetch.cpp
 * @brief LGC source file: Vertex fetch manager, and pass that uses it
 ***********************************************************************************************************************
 */
#include "lgc/patch/VertexFetch.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-vertex-fetch"

using namespace lgc;
using namespace llvm;

namespace lgc {
class BuilderBase;
class PipelineState;
} // namespace lgc

namespace {

// Represents vertex format info corresponding to vertex attribute format (VkFormat).
struct VertexFormatInfo {
  BufNumFormat nfmt;    // Numeric format of vertex buffer
  BufDataFormat dfmt;   // Data format of vertex buffer
  unsigned numChannels; // Valid number of channels
};

// Represents vertex component info corresponding to vertex data format (BufDataFormat).
//
// NOTE: This info is used by vertex fetch instructions. We split vertex fetch into its per-component fetches when
// the original vertex fetch does not match the hardware requirements (such as vertex attribute offset, vertex
// attribute stride, etc..)
struct VertexCompFormatInfo {
  unsigned vertexByteSize; // Byte size of the vertex
  unsigned compByteSize;   // Byte size of each individual component
  unsigned compCount;      // Component count
  BufDataFmt compDfmt;     // Equivalent data format of each component
};

// =====================================================================================================================
// Vertex fetch manager
class VertexFetchImpl : public VertexFetch {
public:
  VertexFetchImpl(LgcContext *lgcContext);
  VertexFetchImpl(const VertexFetchImpl &) = delete;
  VertexFetchImpl &operator=(const VertexFetchImpl &) = delete;

  // Generate code to fetch a vertex value
  Value *fetchVertex(Type *inputTy, const VertexInputDescription *description, unsigned location, unsigned compIdx,
                     BuilderBase &builder) override;

private:
  void initialize(PipelineState *pipelineState);

  static VertexFormatInfo getVertexFormatInfo(const VertexInputDescription *description);

  // Gets variable corresponding to vertex index
  Value *getVertexIndex() { return m_vertexIndex; }

  // Gets variable corresponding to instance index
  Value *getInstanceIndex() { return m_instanceIndex; }

  static const VertexCompFormatInfo *getVertexComponentFormatInfo(unsigned dfmt);

  unsigned mapVertexFormat(unsigned dfmt, unsigned nfmt) const;

  Value *loadVertexBufferDescriptor(unsigned binding, BuilderBase &builder);

  void addVertexFetchInst(Value *vbDesc, unsigned numChannels, bool is16bitFetch, Value *vbIndex, unsigned offset,
                          unsigned stride, unsigned dfmt, unsigned nfmt, Instruction *insertPos, Value **ppFetch) const;

  bool needPostShuffle(const VertexInputDescription *inputDesc, std::vector<Constant *> &shuffleMask) const;

  bool needPatchA2S(const VertexInputDescription *inputDesc) const;

  bool needSecondVertexFetch(const VertexInputDescription *inputDesc) const;

  LgcContext *m_lgcContext = nullptr;   // LGC context
  LLVMContext *m_context = nullptr;     // LLVM context
  Value *m_vertexBufTablePtr = nullptr; // Vertex buffer table pointer
  Value *m_vertexIndex = nullptr;       // Vertex index
  Value *m_instanceIndex = nullptr;     // Instance index

  static const VertexCompFormatInfo m_vertexCompFormatInfo[]; // Info table of vertex component format
  static const unsigned char m_vertexFormatMapGfx10[][8];     // Info table of vertex format mapping for GFX10

  // Default values for vertex fetch (<4 x i32> or <8 x i32>)
  struct {
    Constant *int8;     // < 0, 0, 0, 1 >
    Constant *int16;    // < 0, 0, 0, 1 >
    Constant *int32;    // < 0, 0, 0, 1 >
    Constant *int64;    // < 0, 0, 0, 0, 0, 0, 0, 1 >
    Constant *float16;  // < 0, 0, 0, 0x3C00 >
    Constant *float32;  // < 0, 0, 0, 0x3F800000 >
    Constant *double64; // < 0, 0, 0, 0, 0, 0, 0, 0x3FF00000 >
  } m_fetchDefaults;
};

} // anonymous namespace

// =====================================================================================================================
// Internal tables

#define VERTEX_FORMAT_UNDEFINED(_format)                                                                               \
  { _format, BUF_NUM_FORMAT_FLOAT, BUF_DATA_FORMAT_INVALID, 0, }

// Initializes info table of vertex component format map
const VertexCompFormatInfo VertexFetchImpl::m_vertexCompFormatInfo[] = {
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BUF_DATA_FORMAT_INVALID
    {1, 1, 1, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8
    {2, 2, 1, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16
    {2, 1, 2, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8_8
    {4, 4, 1, BUF_DATA_FORMAT_32},         // BUF_DATA_FORMAT_32
    {4, 2, 2, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16_16
    {4, 0, 0, BUF_DATA_FORMAT_10_11_11},   // BUF_DATA_FORMAT_10_11_11 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_11_11_10},   // BUF_DATA_FORMAT_11_11_10 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_10_10_10_2}, // BUF_DATA_FORMAT_10_10_10_2 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_2_10_10_10}, // BUF_DATA_FORMAT_2_10_10_10 (Packed)
    {4, 1, 4, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8_8_8_8
    {8, 4, 2, BUF_DATA_FORMAT_32},         // BUF_DATA_FORMAT_32_32
    {8, 2, 4, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16_16_16_16
    {12, 4, 3, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32
    {16, 4, 4, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32_32
};

// clang-format off
const unsigned char VertexFetchImpl::m_vertexFormatMapGfx10[][8] = {
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT

    // BUF_DATA_FORMAT_INVALID
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_8
    {BUF_FORMAT_8_UNORM,
     BUF_FORMAT_8_SNORM,
     BUF_FORMAT_8_USCALED,
     BUF_FORMAT_8_SSCALED,
     BUF_FORMAT_8_UINT,
     BUF_FORMAT_8_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_16
    {BUF_FORMAT_16_UNORM,
     BUF_FORMAT_16_SNORM,
     BUF_FORMAT_16_USCALED,
     BUF_FORMAT_16_SSCALED,
     BUF_FORMAT_16_UINT,
     BUF_FORMAT_16_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_FLOAT},

    // BUF_DATA_FORMAT_8_8
    {BUF_FORMAT_8_8_UNORM,
     BUF_FORMAT_8_8_SNORM,
     BUF_FORMAT_8_8_USCALED,
     BUF_FORMAT_8_8_SSCALED,
     BUF_FORMAT_8_8_UINT,
     BUF_FORMAT_8_8_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_FLOAT},

    // BUF_DATA_FORMAT_16_16
    {BUF_FORMAT_16_16_UNORM,
     BUF_FORMAT_16_16_SNORM,
     BUF_FORMAT_16_16_USCALED,
     BUF_FORMAT_16_16_SSCALED,
     BUF_FORMAT_16_16_UINT,
     BUF_FORMAT_16_16_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_FLOAT},

    // BUF_DATA_FORMAT_10_11_11
    {BUF_FORMAT_10_11_11_UNORM_GFX10,
     BUF_FORMAT_10_11_11_SNORM_GFX10,
     BUF_FORMAT_10_11_11_USCALED_GFX10,
     BUF_FORMAT_10_11_11_SSCALED_GFX10,
     BUF_FORMAT_10_11_11_UINT_GFX10,
     BUF_FORMAT_10_11_11_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_10_11_11_FLOAT_GFX10},

    // BUF_DATA_FORMAT_11_11_10
    {BUF_FORMAT_11_11_10_UNORM_GFX10,
     BUF_FORMAT_11_11_10_SNORM_GFX10,
     BUF_FORMAT_11_11_10_USCALED_GFX10,
     BUF_FORMAT_11_11_10_SSCALED_GFX10,
     BUF_FORMAT_11_11_10_UINT_GFX10,
     BUF_FORMAT_11_11_10_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_11_11_10_FLOAT_GFX10},

    // BUF_DATA_FORMAT_10_10_10_2
    {BUF_FORMAT_10_10_10_2_UNORM_GFX10,
     BUF_FORMAT_10_10_10_2_SNORM_GFX10,
     BUF_FORMAT_10_10_10_2_USCALED_GFX10,
     BUF_FORMAT_10_10_10_2_SSCALED_GFX10,
     BUF_FORMAT_10_10_10_2_UINT_GFX10,
     BUF_FORMAT_10_10_10_2_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_2_10_10_10
    {BUF_FORMAT_2_10_10_10_UNORM_GFX10,
     BUF_FORMAT_2_10_10_10_SNORM_GFX10,
     BUF_FORMAT_2_10_10_10_USCALED_GFX10,
     BUF_FORMAT_2_10_10_10_SSCALED_GFX10,
     BUF_FORMAT_2_10_10_10_UINT_GFX10,
     BUF_FORMAT_2_10_10_10_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_8_8_8_8
    {BUF_FORMAT_8_8_8_8_UNORM_GFX10,
     BUF_FORMAT_8_8_8_8_SNORM_GFX10,
     BUF_FORMAT_8_8_8_8_USCALED_GFX10,
     BUF_FORMAT_8_8_8_8_SSCALED_GFX10,
     BUF_FORMAT_8_8_8_8_UINT_GFX10,
     BUF_FORMAT_8_8_8_8_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32_32
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_FLOAT_GFX10},

    // BUF_DATA_FORMAT_16_16_16_16
    {BUF_FORMAT_16_16_16_16_UNORM_GFX10,
     BUF_FORMAT_16_16_16_16_SNORM_GFX10,
     BUF_FORMAT_16_16_16_16_USCALED_GFX10,
     BUF_FORMAT_16_16_16_16_SSCALED_GFX10,
     BUF_FORMAT_16_16_16_16_UINT_GFX10,
     BUF_FORMAT_16_16_16_16_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_16_16_FLOAT_GFX10},

    // BUF_DATA_FORMAT_32_32_32
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_FLOAT_GFX10},

    // BUF_DATA_FORMAT_32_32_32_32
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_32_FLOAT_GFX10},

    // BUF_DATA_FORMAT_RESERVED_15
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},
};
// clang-format on

char LegacyLowerVertexFetch::ID = 0;

// =====================================================================================================================
// Create the vertex fetch pass
ModulePass *lgc::createLegacyLowerVertexFetch() {
  return new LegacyLowerVertexFetch();
}

// =====================================================================================================================
LegacyLowerVertexFetch::LegacyLowerVertexFetch() : ModulePass(ID) {
}

// =====================================================================================================================
// Run the lower vertex fetch pass on a module
//
// @param [in/out] module : Module
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyLowerVertexFetch::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the lower vertex fetch pass on a module
//
// @param [in/out] module : Module
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerVertexFetch::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the lower vertex fetch pass on a module
//
// @param [in/out] module : Module
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool LowerVertexFetch::runImpl(Module &module, PipelineState *pipelineState) {
  std::unique_ptr<VertexFetch> vertexFetch(VertexFetch::create(pipelineState->getLgcContext()));

  // Gather vertex fetch calls. We can assume they're all in one function, the vertex shader.
  // We can assume that multiple fetches of the same location, component and type have been CSEd.
  SmallVector<CallInst *, 8> vertexFetches;
  BuilderBase builder(module.getContext());
  for (auto &func : module) {
    if (!func.isDeclaration() || !func.getName().startswith(lgcName::InputImportVertex))
      continue;
    for (auto user : func.users())
      vertexFetches.push_back(cast<CallInst>(user));
  }
  if (vertexFetches.empty())
    return false;

  if (!pipelineState->isUnlinked() || !pipelineState->getVertexInputDescriptions().empty()) {
    // Whole-pipeline compilation (or shader compilation where we were given the vertex input descriptions).
    // Lower each vertex fetch.
    for (CallInst *call : vertexFetches) {
      Value *vertex = nullptr;

      // Find the vertex input description.
      unsigned location = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
      unsigned component = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
      const VertexInputDescription *description = pipelineState->findVertexInputDescription(location);

      if (!description) {
        // If we could not find vertex input info matching this location, just return undefined value.
        vertex = UndefValue::get(call->getType());
      } else {
        // Fetch the vertex.
        builder.SetInsertPoint(call);
        vertex = vertexFetch->fetchVertex(call->getType(), description, location, component, builder);
      }

      // Replace and erase this call.
      call->replaceAllUsesWith(vertex);
      call->eraseFromParent();
    }

    return true;
  }

  // Unlinked shader compilation; the linker will add a fetch shader. Here we need to
  // 1. add metadata giving the location, component, type of each vertex fetch;
  // 2. add an input arg for each vertex fetch.
  //
  // First add the metadata.
  SmallVector<VertexFetchInfo, 8> info;
  for (CallInst *call : vertexFetches) {
    unsigned location = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
    unsigned component = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
    info.push_back({location, component, call->getType()});
  }
  pipelineState->getPalMetadata()->addVertexFetchInfo(info);

  // Gather the input arg types.
  SmallVector<Type *, 8> argTys;
  SmallVector<std::string, 8> argNames;
  for (CallInst *call : vertexFetches) {
    Type *ty = call->getType();
    // The return value from the fetch shader needs to use all floats, as the back-end maps an int in the
    // return value as an SGPR rather than a VGPR. For symmetry, we also use all floats here, in the input
    // args to the fetchless vertex shader.
    ty = getVgprTy(ty);
    argTys.push_back(ty);
    argNames.push_back("");
  }

  // Mutate the vertex shader function to add the new args.
  Function *newFunc = addFunctionArgs(vertexFetches[0]->getFunction(), nullptr, argTys, argNames);

  // Hook up each vertex fetch call to the corresponding arg.
  for (unsigned idx = 0; idx != vertexFetches.size(); ++idx) {
    CallInst *call = vertexFetches[idx];
    Value *vertex = newFunc->getArg(idx);
    if (call->getType() != vertex->getType()) {
      // We changed to an all-float type above.
      builder.SetInsertPoint(call);
      Type *elementTy = call->getType()->getScalarType();
      unsigned numElements = vertex->getType()->getPrimitiveSizeInBits() / elementTy->getPrimitiveSizeInBits();
      vertex =
          builder.CreateBitCast(vertex, numElements == 1 ? elementTy : FixedVectorType::get(elementTy, numElements));
      if (call->getType() != vertex->getType()) {
        // The types are now vectors of the same element type but different element counts, or call->getType()
        // is scalar.
        if (auto vecTy = dyn_cast<FixedVectorType>(call->getType())) {
          int indices[] = {0, 1, 2, 3};
          vertex =
              builder.CreateShuffleVector(vertex, vertex, ArrayRef<int>(indices).slice(0, vecTy->getNumElements()));
        } else {
          vertex = builder.CreateExtractElement(vertex, uint64_t(0));
        }
      }
    }
    unsigned location = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
    unsigned component = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
    vertex->setName("vertex" + Twine(location) + "." + Twine(component));
    call->replaceAllUsesWith(vertex);
    call->eraseFromParent();
  }

  return true;
}

// =====================================================================================================================
// Create a VertexFetch
VertexFetch *VertexFetch::create(LgcContext *lgcContext) {
  return new VertexFetchImpl(lgcContext);
}

// =====================================================================================================================
// Constructor
//
// @param context : LLVM context
VertexFetchImpl::VertexFetchImpl(LgcContext *lgcContext)
    : m_lgcContext(lgcContext), m_context(&lgcContext->getContext()) {

  // Initialize default fetch values
  auto zero = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
  auto one = ConstantInt::get(Type::getInt32Ty(*m_context), 1);

  // Int8 (0, 0, 0, 1)
  m_fetchDefaults.int8 = ConstantVector::get({zero, zero, zero, one});

  // Int16 (0, 0, 0, 1)
  m_fetchDefaults.int16 = ConstantVector::get({zero, zero, zero, one});

  // Int (0, 0, 0, 1)
  m_fetchDefaults.int32 = ConstantVector::get({zero, zero, zero, one});

  // Int64 (0, 0, 0, 1)
  m_fetchDefaults.int64 = ConstantVector::get({zero, zero, zero, zero, zero, zero, zero, one});

  // Float16 (0, 0, 0, 1.0)
  const uint16_t float16One = 0x3C00;
  auto float16OneVal = ConstantInt::get(Type::getInt32Ty(*m_context), float16One);
  m_fetchDefaults.float16 = ConstantVector::get({zero, zero, zero, float16OneVal});

  // Float (0.0, 0.0, 0.0, 1.0)
  union {
    float f;
    unsigned u32;
  } floatOne = {1.0f};
  auto floatOneVal = ConstantInt::get(Type::getInt32Ty(*m_context), floatOne.u32);
  m_fetchDefaults.float32 = ConstantVector::get({zero, zero, zero, floatOneVal});

  // Double (0.0, 0.0, 0.0, 1.0)
  union {
    double d;
    unsigned u32[2];
  } doubleOne = {1.0};
  auto doubleOne0 = ConstantInt::get(Type::getInt32Ty(*m_context), doubleOne.u32[0]);
  auto doubleOne1 = ConstantInt::get(Type::getInt32Ty(*m_context), doubleOne.u32[1]);
  m_fetchDefaults.double64 = ConstantVector::get({zero, zero, zero, zero, zero, zero, doubleOne0, doubleOne1});
}

// =====================================================================================================================
// Executes vertex fetch operations based on the specified vertex input type and its location.
//
// @param inputTy : Type of vertex input
// @param description : Vertex input description
// @param location : Vertex input location (only used for an IR name, not for functionality)
// @param compIdx : Index used for vector element indexing
// @param builder : Builder to use to insert vertex fetch instructions
Value *VertexFetchImpl::fetchVertex(Type *inputTy, const VertexInputDescription *description, unsigned location,
                                    unsigned compIdx, BuilderBase &builder) {
  Value *vertex = nullptr;
  Instruction *insertPos = &*builder.GetInsertPoint();
  auto vbDesc = loadVertexBufferDescriptor(description->binding, builder);

  Value *vbIndex = nullptr;
  if (description->inputRate == VertexInputRateVertex) {
    // Use vertex index
    if (!m_vertexIndex) {
      auto savedInsertPoint = builder.saveIP();
      builder.SetInsertPointPastAllocas(insertPos->getFunction());
      m_vertexIndex = ShaderInputs::getVertexIndex(builder, *m_lgcContext);
      builder.restoreIP(savedInsertPoint);
    }
    vbIndex = m_vertexIndex;
  } else {
    if (description->inputRate == VertexInputRateNone) {
      vbIndex = ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder);
    } else if (description->inputRate == VertexInputRateInstance) {
      // Use instance index
      if (!m_instanceIndex) {
        auto savedInsertPoint = builder.saveIP();
        builder.SetInsertPointPastAllocas(insertPos->getFunction());
        m_instanceIndex = ShaderInputs::getInstanceIndex(builder, *m_lgcContext);
        builder.restoreIP(savedInsertPoint);
      }
      vbIndex = m_instanceIndex;
    } else {
      // There is a divisor.
      vbIndex = builder.CreateUDiv(ShaderInputs::getInput(ShaderInput::InstanceId, builder, *m_lgcContext),
                                   builder.getInt32(description->inputRate));
      vbIndex = builder.CreateAdd(vbIndex, ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder));
    }
  }

  Value *vertexFetches[2] = {}; // Two vertex fetch operations might be required
  Value *vertexFetch = nullptr; // Coalesced vector by combining the results of two vertex fetch operations

  VertexFormatInfo formatInfo = getVertexFormatInfo(description);

  const bool is8bitFetch = (inputTy->getScalarSizeInBits() == 8);
  const bool is16bitFetch = (inputTy->getScalarSizeInBits() == 16);

  // Do the first vertex fetch operation
  addVertexFetchInst(vbDesc, formatInfo.numChannels, is16bitFetch, vbIndex, description->offset, description->stride,
                     formatInfo.dfmt, formatInfo.nfmt, insertPos, &vertexFetches[0]);

  // Do post-processing in certain cases
  std::vector<Constant *> shuffleMask;
  bool postShuffle = needPostShuffle(description, shuffleMask);
  bool patchA2S = needPatchA2S(description);
  if (postShuffle || patchA2S) {
    if (postShuffle) {
      // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
      // get the components in the right order.
      assert(shuffleMask.empty() == false);
      vertexFetches[0] =
          new ShuffleVectorInst(vertexFetches[0], vertexFetches[0], ConstantVector::get(shuffleMask), "", insertPos);
    }

    if (patchA2S) {
      assert(cast<FixedVectorType>(vertexFetches[0]->getType())->getNumElements() == 4);

      // Extract alpha channel: %a = extractelement %vf0, 3
      Value *alpha = ExtractElementInst::Create(vertexFetches[0], ConstantInt::get(Type::getInt32Ty(*m_context), 3), "",
                                                insertPos);

      if (formatInfo.nfmt == BufNumFormatSint) {
        // NOTE: For format "SINT 10_10_10_2", vertex fetches incorrectly return the alpha channel as
        // unsigned. We have to manually sign-extend it here by doing a "shl" 30 then an "ashr" 30.

        // %a = shl %a, 30
        alpha = BinaryOperator::CreateShl(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = ashr %a, 30
        alpha = BinaryOperator::CreateAShr(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);
      } else if (formatInfo.nfmt == BufNumFormatSnorm) {
        // NOTE: For format "SNORM 10_10_10_2", vertex fetches incorrectly return the alpha channel
        // as unsigned. We have to somehow remap the values { 0.0, 0.33, 0.66, 1.00 } to { 0.0, 1.0,
        // -1.0, -1.0 } respectively.

        // %a = bitcast %a to f32
        alpha = new BitCastInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = mul %a, 3.0f
        alpha = BinaryOperator::CreateFMul(alpha, ConstantFP::get(Type::getFloatTy(*m_context), 3.0f), "", insertPos);

        // %cond = ugt %a, 1.5f
        auto cond =
            new FCmpInst(insertPos, FCmpInst::FCMP_UGT, alpha, ConstantFP::get(Type::getFloatTy(*m_context), 1.5f), "");

        // %a = select %cond, -1.0f, alpha
        alpha = SelectInst::Create(cond, ConstantFP::get(Type::getFloatTy(*m_context), -1.0f), alpha, "", insertPos);

        // %a = bitcast %a to i32
        alpha = new BitCastInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);
      } else if (formatInfo.nfmt == BufNumFormatSscaled) {
        // NOTE: For format "SSCALED 10_10_10_2", vertex fetches incorrectly return the alpha channel
        // as unsigned. We have to somehow remap the values { 0.0, 1.0, 2.0, 3.0 } to { 0.0, 1.0,
        // -2.0, -1.0 } respectively. We can perform the sign extension here by doing a "fptosi", "shl" 30,
        // "ashr" 30, and finally "sitofp".

        // %a = bitcast %a to float
        alpha = new BitCastInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = fptosi %a to i32
        alpha = new FPToSIInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);

        // %a = shl %a, 30
        alpha = BinaryOperator::CreateShl(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = ashr a, 30
        alpha = BinaryOperator::CreateAShr(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = sitofp %a to float
        alpha = new SIToFPInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = bitcast %a to i32
        alpha = new BitCastInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);
      } else
        llvm_unreachable("Should never be called!");

      // Insert alpha channel: %vf0 = insertelement %vf0, %a, 3
      vertexFetches[0] = InsertElementInst::Create(vertexFetches[0], alpha,
                                                   ConstantInt::get(Type::getInt32Ty(*m_context), 3), "", insertPos);
    }
  }

  // Do the second vertex fetch operation
  const bool secondFetch = needSecondVertexFetch(description);
  if (secondFetch) {
    unsigned numChannels = formatInfo.numChannels;
    unsigned dfmt = formatInfo.dfmt;

    if (description->dfmt == BufDataFormat64_64_64) {
      // Valid number of channels and data format have to be revised
      numChannels = 2;
      dfmt = BUF_DATA_FORMAT_32_32;
    }

    addVertexFetchInst(vbDesc, numChannels, is16bitFetch, vbIndex, description->offset + SizeOfVec4,
                       description->stride, dfmt, formatInfo.nfmt, insertPos, &vertexFetches[1]);
  }

  if (secondFetch) {
    // NOTE: If we performs vertex fetch operations twice, we have to coalesce result values of the two
    // fetch operations and generate a combined one.
    assert(vertexFetches[0] && vertexFetches[1]);
    assert(cast<FixedVectorType>(vertexFetches[0]->getType())->getNumElements() == 4);

    unsigned compCount = cast<FixedVectorType>(vertexFetches[1]->getType())->getNumElements();
    assert(compCount == 2 || compCount == 4); // Should be <2 x i32> or <4 x i32>

    if (compCount == 2) {
      // NOTE: We have to enlarge the second vertex fetch, from <2 x i32> to <4 x i32>. Otherwise,
      // vector shuffle operation could not be performed in that it requires the two vectors have
      // the same types.

      // %vf1 = shufflevector %vf1, %vf1, <0, 1, undef, undef>
      Constant *shuffleMask[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), 0), ConstantInt::get(Type::getInt32Ty(*m_context), 1),
          UndefValue::get(Type::getInt32Ty(*m_context)), UndefValue::get(Type::getInt32Ty(*m_context))};
      vertexFetches[1] =
          new ShuffleVectorInst(vertexFetches[1], vertexFetches[1], ConstantVector::get(shuffleMask), "", insertPos);
    }

    // %vf = shufflevector %vf0, %vf1, <0, 1, 2, 3, 4, 5, ...>
    shuffleMask.clear();
    for (unsigned i = 0; i < 4 + compCount; ++i)
      shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), i));
    vertexFetch =
        new ShuffleVectorInst(vertexFetches[0], vertexFetches[1], ConstantVector::get(shuffleMask), "", insertPos);
  } else
    vertexFetch = vertexFetches[0];

  // Finalize vertex fetch
  Type *basicTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;
  const unsigned bitWidth = basicTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  // Get default fetch values
  Constant *defaults = nullptr;

  if (basicTy->isIntegerTy()) {
    if (bitWidth == 8)
      defaults = m_fetchDefaults.int8;
    else if (bitWidth == 16)
      defaults = m_fetchDefaults.int16;
    else if (bitWidth == 32)
      defaults = m_fetchDefaults.int32;
    else {
      assert(bitWidth == 64);
      defaults = m_fetchDefaults.int64;
    }
  } else if (basicTy->isFloatingPointTy()) {
    if (bitWidth == 16)
      defaults = m_fetchDefaults.float16;
    else if (bitWidth == 32)
      defaults = m_fetchDefaults.float32;
    else {
      assert(bitWidth == 64);
      defaults = m_fetchDefaults.double64;
    }
  } else
    llvm_unreachable("Should never be called!");

  const unsigned defaultCompCount = cast<FixedVectorType>(defaults->getType())->getNumElements();
  std::vector<Value *> defaultValues(defaultCompCount);

  for (unsigned i = 0; i < defaultValues.size(); ++i) {
    defaultValues[i] =
        ExtractElementInst::Create(defaults, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
  }

  // Get vertex fetch values
  const unsigned fetchCompCount =
      vertexFetch->getType()->isVectorTy() ? cast<FixedVectorType>(vertexFetch->getType())->getNumElements() : 1;
  std::vector<Value *> fetchValues(fetchCompCount);

  if (fetchCompCount == 1)
    fetchValues[0] = vertexFetch;
  else {
    for (unsigned i = 0; i < fetchCompCount; ++i) {
      fetchValues[i] =
          ExtractElementInst::Create(vertexFetch, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }
  }

  // Construct vertex fetch results
  const unsigned inputCompCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned vertexCompCount = inputCompCount * (bitWidth == 64 ? 2 : 1);

  std::vector<Value *> vertexValues(vertexCompCount);

  // NOTE: Original component index is based on the basic scalar type.
  compIdx *= (bitWidth == 64 ? 2 : 1);

  // Vertex input might take values from vertex fetch values or default fetch values
  for (unsigned i = 0; i < vertexCompCount; i++) {
    if (compIdx + i < fetchCompCount)
      vertexValues[i] = fetchValues[compIdx + i];
    else if (compIdx + i < defaultCompCount)
      vertexValues[i] = defaultValues[compIdx + i];
    else {
      llvm_unreachable("Should never be called!");
      vertexValues[i] = UndefValue::get(Type::getInt32Ty(*m_context));
    }
  }

  if (vertexCompCount == 1)
    vertex = vertexValues[0];
  else {
    Type *vertexTy = FixedVectorType::get(Type::getInt32Ty(*m_context), vertexCompCount);
    vertex = UndefValue::get(vertexTy);

    for (unsigned i = 0; i < vertexCompCount; ++i) {
      vertex = InsertElementInst::Create(vertex, vertexValues[i], ConstantInt::get(Type::getInt32Ty(*m_context), i), "",
                                         insertPos);
    }
  }

  if (is8bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i32> now. For 8-bit vertex fetch, we have to
    // convert them to <n x i8> and the 24 high bits is truncated.
    assert(inputTy->isIntOrIntVectorTy()); // Must be integer type

    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt8Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(FixedVectorType::get(truncTy, cast<FixedVectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = new TruncInst(vertex, truncTy, "", insertPos);
  } else if (is16bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i32> now. For 16-bit vertex fetch, we have to
    // convert them to <n x i16> and the 16 high bits is truncated.
    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt16Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(FixedVectorType::get(truncTy, cast<FixedVectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = new TruncInst(vertex, truncTy, "", insertPos);
  }

  if (vertex->getType() != inputTy)
    vertex = new BitCastInst(vertex, inputTy, "", insertPos);
  vertex->setName("vertex" + Twine(location) + "." + Twine(compIdx));

  return vertex;
}

// =====================================================================================================================
// Gets info from table according to vertex attribute format.
//
// @param inputDesc : Vertex input description
VertexFormatInfo VertexFetchImpl::getVertexFormatInfo(const VertexInputDescription *inputDesc) {
  VertexFormatInfo info = {static_cast<BufNumFormat>(inputDesc->nfmt), static_cast<BufDataFormat>(inputDesc->dfmt), 1};
  switch (inputDesc->dfmt) {
  case BufDataFormat8_8:
  case BufDataFormat16_16:
  case BufDataFormat32_32:
    info.numChannels = 2;
    break;
  case BufDataFormat32_32_32:
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
    info.numChannels = 3;
    break;
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
    info.numChannels = 4;
    break;
  case BufDataFormat8_8_8_8_Bgra:
    info.numChannels = 4;
    info.dfmt = BufDataFormat8_8_8_8;
    break;
  case BufDataFormat2_10_10_10_Bgra:
    info.numChannels = 4;
    info.dfmt = BufDataFormat2_10_10_10;
    break;
  case BufDataFormat64:
    info.numChannels = 2;
    info.dfmt = BufDataFormat32_32;
    break;
  case BufDataFormat64_64:
  case BufDataFormat64_64_64:
  case BufDataFormat64_64_64_64:
    info.numChannels = 4;
    info.dfmt = BufDataFormat32_32_32_32;
    break;
  default:
    break;
  }
  return info;
}

// =====================================================================================================================
// Gets component info from table according to vertex buffer data format.
//
// @param dfmt : Date format of vertex buffer
const VertexCompFormatInfo *VertexFetchImpl::getVertexComponentFormatInfo(unsigned dfmt) {
  assert(dfmt < sizeof(m_vertexCompFormatInfo) / sizeof(m_vertexCompFormatInfo[0]));
  return &m_vertexCompFormatInfo[dfmt];
}

// =====================================================================================================================
// Maps separate buffer data and numeric formats to the combined buffer format
//
// @param dfmt : Data format
// @param nfmt : Numeric format
unsigned VertexFetchImpl::mapVertexFormat(unsigned dfmt, unsigned nfmt) const {
  assert(dfmt < 16);
  assert(nfmt < 8);
  unsigned format = 0;

  GfxIpVersion gfxIp = m_lgcContext->getTargetInfo().getGfxIpVersion();
  switch (gfxIp.major) {
  default: {
    CombineFormat formatOprd = {};
    formatOprd.bits.dfmt = dfmt;
    formatOprd.bits.nfmt = nfmt;
    format = formatOprd.u32All;
    break;
  }
  case 10:
    assert(dfmt < sizeof(m_vertexFormatMapGfx10) / sizeof(m_vertexFormatMapGfx10[0]));
    assert(nfmt < sizeof(m_vertexFormatMapGfx10[0]) / sizeof(m_vertexFormatMapGfx10[0][0]));
    format = m_vertexFormatMapGfx10[dfmt][nfmt];
    break;
  }
  return format;
}

// =====================================================================================================================
// Loads vertex descriptor based on the specified vertex input location.
//
// @param binding : ID of vertex buffer binding
// @param builder : Builder with insert point set
Value *VertexFetchImpl::loadVertexBufferDescriptor(unsigned binding, BuilderBase &builder) {

  // Get the vertex buffer table pointer as pointer to v4i32 descriptor.
  Type *vbDescTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
  if (!m_vertexBufTablePtr) {
    auto savedInsertPoint = builder.saveIP();
    builder.SetInsertPointPastAllocas(builder.GetInsertPoint()->getFunction());
    m_vertexBufTablePtr =
        ShaderInputs::getSpecialUserDataAsPointer(UserDataMapping::VertexBufferTable, vbDescTy, builder);
    builder.restoreIP(savedInsertPoint);
  }

  Value *vbDescPtr = builder.CreateGEP(vbDescTy, m_vertexBufTablePtr, builder.getInt64(binding));
  LoadInst *vbDesc = builder.CreateLoad(vbDescTy, vbDescPtr);
  vbDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(vbDesc->getContext(), {}));
  vbDesc->setAlignment(Align(16));
  return vbDesc;
}

// =====================================================================================================================
// Inserts instructions to do vertex fetch operations.
//
// The stride is passed only to ensure that a valid load is used, not to actually calculate the load address.
// Instead, we use the index as the index in a structured tbuffer load instruction, and rely on the driver
// setting up the descriptor with the correct stride.
//
// @param vbDesc : Vertex buffer descriptor
// @param numChannels : Valid number of channels
// @param is16bitFetch : Whether it is 16-bit vertex fetch
// @param vbIndex : Index of vertex fetch in buffer
// @param offset : Vertex attribute offset (in bytes)
// @param stride : Vertex attribute stride (in bytes)
// @param dfmt : Date format of vertex buffer
// @param nfmt : Numeric format of vertex buffer
// @param insertPos : Where to insert instructions
// @param [out] ppFetch : Destination of vertex fetch
void VertexFetchImpl::addVertexFetchInst(Value *vbDesc, unsigned numChannels, bool is16bitFetch, Value *vbIndex,
                                         unsigned offset, unsigned stride, unsigned dfmt, unsigned nfmt,
                                         Instruction *insertPos, Value **ppFetch) const {
  const VertexCompFormatInfo *formatInfo = getVertexComponentFormatInfo(dfmt);

  // NOTE: If the vertex attribute offset and stride are aligned on data format boundaries, we can do a vertex fetch
  // operation to read the whole vertex. Otherwise, we have to do vertex per-component fetch operations.
  if (((offset % formatInfo->vertexByteSize) == 0 && (stride % formatInfo->vertexByteSize) == 0 &&
       // NOTE: For the vertex data format 8_8, 8_8_8_8, 16_16, and 16_16_16_16, tbuffer_load has a HW defect when
       // vertex buffer is unaligned. Therefore, we have to split the vertex fetch to component-based ones
       dfmt != BufDataFormat8_8 && dfmt != BufDataFormat8_8_8_8 && dfmt != BufDataFormat16_16 &&
       dfmt != BufDataFormat16_16_16_16) ||
      formatInfo->compDfmt == dfmt) {
    // NOTE: If the vertex attribute offset is greater than vertex attribute stride, we have to adjust both vertex
    // buffer index and vertex attribute offset accordingly. Otherwise, vertex fetch might behave unexpectedly.
    if (stride != 0 && offset > stride) {
      vbIndex = BinaryOperator::CreateAdd(vbIndex, ConstantInt::get(Type::getInt32Ty(*m_context), offset / stride), "",
                                          insertPos);
      offset = offset % stride;
    }

    // Do vertex fetch
    Value *args[] = {
        vbDesc,                                                                      // rsrc
        vbIndex,                                                                     // vindex
        ConstantInt::get(Type::getInt32Ty(*m_context), offset),                      // offset
        ConstantInt::get(Type::getInt32Ty(*m_context), 0),                           // soffset
        ConstantInt::get(Type::getInt32Ty(*m_context), mapVertexFormat(dfmt, nfmt)), // dfmt, nfmt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0)                            // glc, slc
    };

    StringRef suffix = "";
    Type *fetchTy = nullptr;

    if (is16bitFetch) {
      switch (numChannels) {
      case 1:
        suffix = ".f16";
        fetchTy = Type::getHalfTy(*m_context);
        break;
      case 2:
        suffix = ".v2f16";
        fetchTy = FixedVectorType::get(Type::getHalfTy(*m_context), 2);
        break;
      case 3:
      case 4:
        suffix = ".v4f16";
        fetchTy = FixedVectorType::get(Type::getHalfTy(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    } else {
      switch (numChannels) {
      case 1:
        suffix = ".i32";
        fetchTy = Type::getInt32Ty(*m_context);
        break;
      case 2:
        suffix = ".v2i32";
        fetchTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
        break;
      case 3:
      case 4:
        suffix = ".v4i32";
        fetchTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    }

    Value *fetch = emitCall((Twine("llvm.amdgcn.struct.tbuffer.load") + suffix).str(), fetchTy, args, {}, insertPos);

    if (is16bitFetch) {
      // NOTE: The fetch values are represented by <n x i32>, so we will bitcast the float16 values to
      // int32 eventually.
      Type *bitCastTy = Type::getInt16Ty(*m_context);
      bitCastTy = numChannels == 1 ? bitCastTy : FixedVectorType::get(bitCastTy, numChannels);
      fetch = new BitCastInst(fetch, bitCastTy, "", insertPos);

      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = numChannels == 1 ? zExtTy : FixedVectorType::get(zExtTy, numChannels);
      fetch = new ZExtInst(fetch, zExtTy, "", insertPos);
    }

    if (numChannels == 3) {
      // NOTE: If valid number of channels is 3, the actual fetch type should be revised from <4 x i32>
      // to <3 x i32>.
      Constant *shuffleMask[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 2)};
      *ppFetch = new ShuffleVectorInst(fetch, fetch, ConstantVector::get(shuffleMask), "", insertPos);
    } else
      *ppFetch = fetch;
  } else {
    // NOTE: Here, we split the vertex into its components and do per-component fetches. The expectation
    // is that the vertex per-component fetches always match the hardware requirements.
    assert(numChannels == formatInfo->compCount);

    Value *compVbIndices[4] = {};
    unsigned compOffsets[4] = {};

    for (unsigned i = 0; i < formatInfo->compCount; ++i) {
      unsigned compOffset = offset + i * formatInfo->compByteSize;

      // NOTE: If the vertex attribute per-component offset is greater than vertex attribute stride, we have
      // to adjust both vertex buffer index and vertex per-component offset accordingly. Otherwise, vertex
      // fetch might behave unexpectedly.
      if (stride != 0 && compOffset > stride) {
        compVbIndices[i] = BinaryOperator::CreateAdd(
            vbIndex, ConstantInt::get(Type::getInt32Ty(*m_context), compOffset / stride), "", insertPos);
        compOffsets[i] = compOffset % stride;
      } else {
        compVbIndices[i] = vbIndex;
        compOffsets[i] = compOffset;
      }
    }

    Type *fetchTy = FixedVectorType::get(Type::getInt32Ty(*m_context), numChannels);
    Value *fetch = UndefValue::get(fetchTy);

    // Do vertex per-component fetches
    for (unsigned i = 0; i < formatInfo->compCount; ++i) {
      Value *args[] = {
          vbDesc,                                                                                      // rsrc
          compVbIndices[i],                                                                            // vindex
          ConstantInt::get(Type::getInt32Ty(*m_context), compOffsets[i]),                              // offset
          ConstantInt::get(Type::getInt32Ty(*m_context), 0),                                           // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), mapVertexFormat(formatInfo->compDfmt, nfmt)), // dfmt, nfmt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0)                                            // glc, slc
      };

      Value *compFetch = nullptr;
      if (is16bitFetch) {
        compFetch = emitCall("llvm.amdgcn.struct.tbuffer.load.f16", Type::getHalfTy(*m_context), args, {}, insertPos);

        compFetch = new BitCastInst(compFetch, Type::getInt16Ty(*m_context), "", insertPos);
        compFetch = new ZExtInst(compFetch, Type::getInt32Ty(*m_context), "", insertPos);
      } else {
        compFetch = emitCall("llvm.amdgcn.struct.tbuffer.load.i32", Type::getInt32Ty(*m_context), args, {}, insertPos);
      }

      fetch =
          InsertElementInst::Create(fetch, compFetch, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }

    *ppFetch = fetch;
  }
}

// =====================================================================================================================
// Checks whether post shuffle is required for vertex fetch operation.
//
// @param inputDesc : Vertex input description
// @param [out] shuffleMask : Vector shuffle mask
bool VertexFetchImpl::needPostShuffle(const VertexInputDescription *inputDesc,
                                      std::vector<Constant *> &shuffleMask) const {
  bool needShuffle = false;

  switch (inputDesc->dfmt) {
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 2));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 1));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 3));
    needShuffle = true;
    break;
  default:
    break;
  }

  return needShuffle;
}

// =====================================================================================================================
// Checks whether patching 2-bit signed alpha channel is required for vertex fetch operation.
//
// @param inputDesc : Vertex input description
bool VertexFetchImpl::needPatchA2S(const VertexInputDescription *inputDesc) const {
  bool needPatch = false;

  if (inputDesc->dfmt == BufDataFormat2_10_10_10 || inputDesc->dfmt == BufDataFormat2_10_10_10_Bgra) {
    if (inputDesc->nfmt == BufNumFormatSnorm || inputDesc->nfmt == BufNumFormatSscaled ||
        inputDesc->nfmt == BufNumFormatSint)
      needPatch = m_lgcContext->getTargetInfo().getGfxIpVersion().major < 9;
  }

  return needPatch;
}

// =====================================================================================================================
// Checks whether the second vertex fetch operation is required (particularly for certain 64-bit typed formats).
//
// @param inputDesc : Vertex input description
bool VertexFetchImpl::needSecondVertexFetch(const VertexInputDescription *inputDesc) const {
  return inputDesc->dfmt == BufDataFormat64_64_64 || inputDesc->dfmt == BufDataFormat64_64_64_64;
}

// =====================================================================================================================
// Initialize the lower vertex fetch pass
INITIALIZE_PASS(LegacyLowerVertexFetch, DEBUG_TYPE, "Lower vertex fetch calls", false, false)
