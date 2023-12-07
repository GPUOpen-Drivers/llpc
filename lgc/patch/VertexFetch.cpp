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
 * @file  VertexFetch.cpp
 * @brief LGC source file: Vertex fetch manager, and pass that uses it
 ***********************************************************************************************************************
 */
#include "lgc/patch/VertexFetch.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-vertex-fetch"

using namespace lgc;
using namespace llvm;

namespace lgc {
class BuilderBase;
class PipelineState;
} // namespace lgc

namespace {

// Map vkgc
static constexpr unsigned InternalDescriptorSetId = static_cast<unsigned>(-1);
static constexpr unsigned FetchShaderInternalBufferBinding = 5; // Descriptor binding for uber fetch shader
static constexpr unsigned CurrentAttributeBufferBinding = 1;    // Descriptor binding for current attribute
static constexpr unsigned GenericVertexFetchShaderBinding = 0;  // Descriptor binding for generic vertex fetch shader
static constexpr unsigned VertexInputBindingCurrent = 64;       // Vertex input binding for current attribute

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
  VertexFetchImpl(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors);
  VertexFetchImpl(const VertexFetchImpl &) = delete;
  VertexFetchImpl &operator=(const VertexFetchImpl &) = delete;

  // Generate code to fetch a vertex value
  Value *fetchVertex(Type *inputTy, const VertexInputDescription *description, unsigned location, unsigned compIdx,
                     BuilderImpl &builderImpl) override;

  // Generate code to fetch a vertex value for uber shader
  Value *fetchVertex(InputImportGenericOp *inst, Value *descPtr, Value *locMasks, BuilderBase &builder) override;

private:
  static VertexFormatInfo getVertexFormatInfo(const VertexInputDescription *description);

  // Gets variable corresponding to vertex index
  Value *getVertexIndex() { return m_vertexIndex; }

  // Gets variable corresponding to instance index
  Value *getInstanceIndex() { return m_instanceIndex; }

  static const VertexCompFormatInfo *getVertexComponentFormatInfo(unsigned dfmt);

  unsigned mapVertexFormat(unsigned dfmt, unsigned nfmt) const;

  Value *loadVertexBufferDescriptor(unsigned binding, BuilderImpl &builderImpl);

  void addVertexFetchInst(Value *vbDesc, unsigned numChannels, bool is16bitFetch, Value *vbIndex, Value *srdStride,
                          unsigned offset, unsigned stride, unsigned dfmt, unsigned nfmt, Instruction *insertPos,
                          Value **ppFetch) const;

  bool needPostShuffle(const VertexInputDescription *inputDesc, std::vector<Constant *> &shuffleMask) const;

  bool needSecondVertexFetch(const VertexInputDescription *inputDesc) const;

  bool needPatch32(const VertexInputDescription *inputDesc) const;

  std::pair<Value *, Value *> convertSrdToOffsetMode(Value *vbDesc, BuilderBase &builder);

  LgcContext *m_lgcContext = nullptr;      // LGC context
  LLVMContext *m_context = nullptr;        // LLVM context
  Value *m_vertexBufTablePtr = nullptr;    // Vertex buffer table pointer
  Value *m_curAttribBufferDescr = nullptr; // Current attribute buffer descriptor;
  Value *m_vertexIndex = nullptr;          // Vertex index
  Value *m_instanceIndex = nullptr;        // Instance index

  bool m_useSoftwareVertexBufferDescriptors = false; // Use software vertex buffer descriptors to structure SRD.

  static const VertexCompFormatInfo m_vertexCompFormatInfo[]; // Info table of vertex component format
  static const unsigned char m_vertexFormatMapGfx10[][9];     // Info table of vertex format mapping for GFX10
  static const unsigned char m_vertexFormatMapGfx11[][9];     // Info table of vertex format mapping for GFX11

  // Default values for vertex fetch (<4 x i32> or <8 x i32>)
  struct {
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
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormatReserved
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat8_8_8_8_Bgra
    {3, 1, 3, BUF_DATA_FORMAT_8},          // BufDataFormat8_8_8
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat8_8_8_Bgr,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat2_10_10_10_Bgra,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat64,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat64_64,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat64_64_64,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat64_64_64_64,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat4_4,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat4_4_4_4,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat4_4_4_4_Bgra,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat5_6_5,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat5_6_5_Bgr,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat5_6_5_1,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat5_6_5_1_Bgra,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat1_5_6_5,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat5_9_9_9,
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormat8_A,
    {6, 2, 3, BUF_DATA_FORMAT_16},         // BufDataFormat16_16_16
};

// clang-format off
const unsigned char VertexFetchImpl::m_vertexFormatMapGfx10[][9] = {
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT
    //   BUF_NUM_FORMAT_FIXED

    // BUF_DATA_FORMAT_INVALID
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
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
     BUF_FORMAT_16_FLOAT,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_8_8
    {BUF_FORMAT_8_8_UNORM,
     BUF_FORMAT_8_8_SNORM,
     BUF_FORMAT_8_8_USCALED,
     BUF_FORMAT_8_8_SSCALED,
     BUF_FORMAT_8_8_UINT,
     BUF_FORMAT_8_8_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32
    {BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_FLOAT,
     BUF_FORMAT_32_SINT},

    // BUF_DATA_FORMAT_16_16
    {BUF_FORMAT_16_16_UNORM,
     BUF_FORMAT_16_16_SNORM,
     BUF_FORMAT_16_16_USCALED,
     BUF_FORMAT_16_16_SSCALED,
     BUF_FORMAT_16_16_UINT,
     BUF_FORMAT_16_16_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_FLOAT,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_10_11_11
    {BUF_FORMAT_10_11_11_UNORM_GFX10,
     BUF_FORMAT_10_11_11_SNORM_GFX10,
     BUF_FORMAT_10_11_11_USCALED_GFX10,
     BUF_FORMAT_10_11_11_SSCALED_GFX10,
     BUF_FORMAT_10_11_11_UINT_GFX10,
     BUF_FORMAT_10_11_11_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_10_11_11_FLOAT_GFX10,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_11_11_10
    {BUF_FORMAT_11_11_10_UNORM_GFX10,
     BUF_FORMAT_11_11_10_SNORM_GFX10,
     BUF_FORMAT_11_11_10_USCALED_GFX10,
     BUF_FORMAT_11_11_10_SSCALED_GFX10,
     BUF_FORMAT_11_11_10_UINT_GFX10,
     BUF_FORMAT_11_11_10_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_11_11_10_FLOAT_GFX10,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_10_10_10_2
    {BUF_FORMAT_10_10_10_2_UNORM_GFX10,
     BUF_FORMAT_10_10_10_2_SNORM_GFX10,
     BUF_FORMAT_10_10_10_2_USCALED_GFX10,
     BUF_FORMAT_10_10_10_2_SSCALED_GFX10,
     BUF_FORMAT_10_10_10_2_UINT_GFX10,
     BUF_FORMAT_10_10_10_2_SINT_GFX10,
     BUF_FORMAT_INVALID,
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
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32_32
    {BUF_FORMAT_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_FLOAT_GFX10,
     BUF_FORMAT_32_32_SINT_GFX10},

    // BUF_DATA_FORMAT_16_16_16_16
    {BUF_FORMAT_16_16_16_16_UNORM_GFX10,
     BUF_FORMAT_16_16_16_16_SNORM_GFX10,
     BUF_FORMAT_16_16_16_16_USCALED_GFX10,
     BUF_FORMAT_16_16_16_16_SSCALED_GFX10,
     BUF_FORMAT_16_16_16_16_UINT_GFX10,
     BUF_FORMAT_16_16_16_16_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_16_16_FLOAT_GFX10,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32_32_32
    {BUF_FORMAT_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_FLOAT_GFX10,
     BUF_FORMAT_32_32_32_SINT_GFX10},

    // BUF_DATA_FORMAT_32_32_32_32
    {BUF_FORMAT_32_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_32_SINT_GFX10,
     BUF_FORMAT_32_32_32_32_UINT_GFX10,
     BUF_FORMAT_32_32_32_32_SINT_GFX10,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_32_FLOAT_GFX10,
     BUF_FORMAT_32_32_32_32_SINT_GFX10},

    // BUF_DATA_FORMAT_RESERVED_15
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},
};
// clang-format on

// clang-format off
const unsigned char VertexFetchImpl::m_vertexFormatMapGfx11[][9] = {
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT
    //   BUF_NUM_FORMAT_FIXED

    // BUF_DATA_FORMAT_INVALID
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
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
     BUF_FORMAT_16_FLOAT,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_8_8
    {BUF_FORMAT_8_8_UNORM,
     BUF_FORMAT_8_8_SNORM,
     BUF_FORMAT_8_8_USCALED,
     BUF_FORMAT_8_8_SSCALED,
     BUF_FORMAT_8_8_UINT,
     BUF_FORMAT_8_8_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32
    {BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_32_UINT,
     BUF_FORMAT_32_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_FLOAT,
     BUF_FORMAT_32_SINT},

    // BUF_DATA_FORMAT_16_16
    {BUF_FORMAT_16_16_UNORM,
     BUF_FORMAT_16_16_SNORM,
     BUF_FORMAT_16_16_USCALED,
     BUF_FORMAT_16_16_SSCALED,
     BUF_FORMAT_16_16_UINT,
     BUF_FORMAT_16_16_SINT,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_FLOAT,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_10_11_11
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_10_11_11_FLOAT_GFX11,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_11_11_10
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_11_11_10_FLOAT_GFX11,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_10_10_10_2
    {BUF_FORMAT_10_10_10_2_UNORM_GFX11,
     BUF_FORMAT_10_10_10_2_SNORM_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_10_10_10_2_UINT_GFX11,
     BUF_FORMAT_10_10_10_2_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_2_10_10_10
    {BUF_FORMAT_2_10_10_10_UNORM_GFX11,
     BUF_FORMAT_2_10_10_10_SNORM_GFX11,
     BUF_FORMAT_2_10_10_10_USCALED_GFX11,
     BUF_FORMAT_2_10_10_10_SSCALED_GFX11,
     BUF_FORMAT_2_10_10_10_UINT_GFX11,
     BUF_FORMAT_2_10_10_10_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_8_8_8_8
    {BUF_FORMAT_8_8_8_8_UNORM_GFX11,
     BUF_FORMAT_8_8_8_8_SNORM_GFX11,
     BUF_FORMAT_8_8_8_8_USCALED_GFX11,
     BUF_FORMAT_8_8_8_8_SSCALED_GFX11,
     BUF_FORMAT_8_8_8_8_UINT_GFX11,
     BUF_FORMAT_8_8_8_8_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32_32
    {BUF_FORMAT_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_FLOAT_GFX11,
     BUF_FORMAT_32_32_SINT_GFX11},

    // BUF_DATA_FORMAT_16_16_16_16
    {BUF_FORMAT_16_16_16_16_UNORM_GFX11,
     BUF_FORMAT_16_16_16_16_SNORM_GFX11,
     BUF_FORMAT_16_16_16_16_USCALED_GFX11,
     BUF_FORMAT_16_16_16_16_SSCALED_GFX11,
     BUF_FORMAT_16_16_16_16_UINT_GFX11,
     BUF_FORMAT_16_16_16_16_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_16_16_16_16_FLOAT_GFX11,
     BUF_FORMAT_INVALID},

    // BUF_DATA_FORMAT_32_32_32
    {BUF_FORMAT_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_FLOAT_GFX11,
     BUF_FORMAT_32_32_32_SINT_GFX11},

    // BUF_DATA_FORMAT_32_32_32_32
    {BUF_FORMAT_32_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_32_SINT_GFX11,
     BUF_FORMAT_32_32_32_32_UINT_GFX11,
     BUF_FORMAT_32_32_32_32_SINT_GFX11,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_32_32_32_32_FLOAT_GFX11,
     BUF_FORMAT_32_32_32_32_SINT_GFX11},

    // BUF_DATA_FORMAT_RESERVED_15
    {BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID,
     BUF_FORMAT_INVALID},
};
// clang-format on

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
  // Gather vertex fetch calls. We can assume they're all in one function, the vertex shader.
  // We can assume that multiple fetches of the same location, component and type have been CSEd.
  SmallVector<InputImportGenericOp *, 8> vertexFetches;
  static const auto fetchVisitor = llvm_dialects::VisitorBuilder<SmallVectorImpl<InputImportGenericOp *>>()
                                       .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                       .add<InputImportGenericOp>([](auto &fetches, InputImportGenericOp &op) {
                                         if (lgc::getShaderStage(op.getFunction()) == ShaderStageVertex)
                                           fetches.push_back(&op);
                                       })
                                       .build();
  fetchVisitor.visit(vertexFetches, module);
  if (vertexFetches.empty())
    return false;

  std::unique_ptr<VertexFetch> vertexFetch(VertexFetch::create(
      pipelineState->getLgcContext(), pipelineState->getOptions().useSoftwareVertexBufferDescriptors));
  BuilderImpl builder(pipelineState);

  if (pipelineState->getOptions().enableUberFetchShader) {
    // NOTE: The 10_10_10_2 formats are not supported by the uber fetch shader on gfx9 and older.
    // We rely on the driver to fallback to not using the uber fetch shader when those formats are used.
    builder.setShaderStage(ShaderStageVertex);
    builder.SetInsertPointPastAllocas(vertexFetches[0]->getFunction());
    auto desc = builder.CreateLoadBufferDesc(InternalDescriptorSetId, FetchShaderInternalBufferBinding,
                                             builder.getInt32(0), Builder::BufferFlagAddress);

    auto descPtr = builder.CreateIntToPtr(desc, builder.getPtrTy(ADDR_SPACE_CONST));

    // 64 bit location masks.
    Value *locationMasks = builder.CreateLoad(builder.getInt64Ty(), descPtr);
    descPtr = builder.CreateGEP(builder.getInt64Ty(), descPtr, {builder.getInt32(1)});

    for (InputImportGenericOp *inst : vertexFetches) {
      builder.SetInsertPoint(inst);
      Value *vertex = vertexFetch->fetchVertex(inst, descPtr, locationMasks, BuilderBase::get(builder));
      // Replace and erase this instruction.
      inst->replaceAllUsesWith(vertex);
      inst->eraseFromParent();
    }
    return true;
  }

  if (!pipelineState->isUnlinked() || !pipelineState->getVertexInputDescriptions().empty()) {
    // Whole-pipeline compilation (or shader compilation where we were given the vertex input descriptions).
    // Lower each vertex fetch.
    for (InputImportGenericOp *fetch : vertexFetches) {
      Value *vertex = nullptr;

      // Find the vertex input description.
      unsigned location = fetch->getLocation();
      unsigned component = cast<ConstantInt>(fetch->getElemIdx())->getZExtValue();

      assert(!fetch->getPerPrimitive());
      assert(cast<ConstantInt>(fetch->getLocOffset())->isZero());

      const VertexInputDescription *description = pipelineState->findVertexInputDescription(location);

      if (!description) {
        // If we could not find vertex input info matching this location, just return undefined value.
        vertex = PoisonValue::get(fetch->getType());
      } else {
        // Fetch the vertex.
        builder.SetInsertPoint(fetch);
        builder.setShaderStage(ShaderStageVertex);
        vertex = vertexFetch->fetchVertex(fetch->getType(), description, location, component, builder);
      }

      // Replace and erase this call.
      fetch->replaceAllUsesWith(vertex);
      fetch->eraseFromParent();
    }

    return true;
  }

  // Unlinked shader compilation; the linker will add a fetch shader. Here we need to
  // 1. add metadata giving the location, component, type of each vertex fetch;
  // 2. add an input arg for each vertex fetch.
  //
  // First add the metadata and mutate the vertex shader function.
  SmallVector<VertexFetchInfo, 8> info;
  SmallVector<Type *, 8> argTys;
  SmallVector<std::string, 8> argNames;
  for (InputImportGenericOp *fetch : vertexFetches) {
    unsigned location = fetch->getLocation();
    unsigned component = cast<ConstantInt>(fetch->getElemIdx())->getZExtValue();

    assert(!fetch->getPerPrimitive());
    assert(cast<ConstantInt>(fetch->getLocOffset())->isZero());

    info.push_back({location, component, fetch->getType()});

    Type *ty = fetch->getType();
    // The return value from the fetch shader needs to use all floats, as the back-end maps an int in the
    // return value as an SGPR rather than a VGPR. For symmetry, we also use all floats here, in the input
    // args to the fetchless vertex shader.
    ty = getVgprTy(ty);
    argTys.push_back(ty);
    argNames.push_back("");
  }
  pipelineState->getPalMetadata()->addVertexFetchInfo(info);

  // Mutate the vertex shader function to add the new args.
  Function *newFunc = addFunctionArgs(vertexFetches[0]->getFunction(), nullptr, argTys, argNames);

  // Hook up each vertex fetch to the corresponding arg.
  for (unsigned idx = 0; idx != vertexFetches.size(); ++idx) {
    InputImportGenericOp *fetch = vertexFetches[idx];
    Value *vertex = newFunc->getArg(idx);
    if (fetch->getType() != vertex->getType()) {
      // We changed to an all-float type above.
      builder.SetInsertPoint(fetch);
      Type *elementTy = fetch->getType()->getScalarType();
      unsigned numElements = vertex->getType()->getPrimitiveSizeInBits() / elementTy->getPrimitiveSizeInBits();
      vertex =
          builder.CreateBitCast(vertex, numElements == 1 ? elementTy : FixedVectorType::get(elementTy, numElements));
      if (fetch->getType() != vertex->getType()) {
        // The types are now vectors of the same element type but different element counts, or fetch->getType()
        // is scalar.
        if (auto vecTy = dyn_cast<FixedVectorType>(fetch->getType())) {
          int indices[] = {0, 1, 2, 3};
          vertex =
              builder.CreateShuffleVector(vertex, vertex, ArrayRef<int>(indices).slice(0, vecTy->getNumElements()));
        } else {
          vertex = builder.CreateExtractElement(vertex, uint64_t(0));
        }
      }
    }
    vertex->setName("vertex" + Twine(info[idx].location) + "." + Twine(info[idx].component));
    fetch->replaceAllUsesWith(vertex);
    fetch->eraseFromParent();
  }

  return true;
}

// =====================================================================================================================
// This is an lgc.input.import.generic operation for vertex buffers.
// Executes vertex fetch operations based on the uber shader buffer.
//
// @param inst : the input instruction
// @param descPtr : 64bit address of buffer
// @param locMasks : determine if the attribute data is valid.
// @param builder : Builder to use to insert vertex fetch instructions
// @returns : vertex
Value *VertexFetchImpl::fetchVertex(InputImportGenericOp *inst, Value *descPtr, Value *locMasks, BuilderBase &builder) {
  if (!m_vertexIndex) {
    IRBuilderBase::InsertPointGuard ipg(builder);
    builder.SetInsertPointPastAllocas(inst->getFunction());
    m_vertexIndex = ShaderInputs::getVertexIndex(builder, *m_lgcContext);
  }

  if (!m_instanceIndex) {
    IRBuilderBase::InsertPointGuard ipg(builder);
    builder.SetInsertPointPastAllocas(inst->getFunction());
    m_instanceIndex = ShaderInputs::getInstanceIndex(builder, *m_lgcContext);
  }

  // Get the vertex buffer table pointer as pointer to v4i32 descriptor.
  Type *vbDescTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
  if (!m_vertexBufTablePtr) {
    IRBuilderBase::InsertPointGuard ipg(builder);
    builder.SetInsertPointPastAllocas(inst->getFunction());
    m_vertexBufTablePtr =
        ShaderInputs::getSpecialUserDataAsPointer(UserDataMapping::VertexBufferTable, vbDescTy, builder);
  }

  // Helper to create basic block
  auto createBlock = [&](Twine blockName, BasicBlock *bb) {
    return BasicBlock::Create(*m_context, blockName, &*inst->getFunction(), bb);
  };

  auto currentBlock = inst->getParent();
  auto fetchEndBlock = currentBlock->splitBasicBlock(inst);

  auto fetchUberEndBlock = createBlock(".fetchUberEndBlock", fetchEndBlock);
  auto perCompEndBlock = createBlock(".perCompEnd", fetchUberEndBlock);
  auto comp3Block = createBlock(".comp3Block", perCompEndBlock);
  auto comp2Block = createBlock(".comp2Block", comp3Block);
  auto comp1Block = createBlock(".comp1Block", comp2Block);
  auto comp0Block = createBlock(".comp0Block", comp1Block);
  auto wholeVertexBlock = createBlock(".wholeVertex", comp0Block);
  auto fetchUberStartBlock = createBlock(".fetchUberStartBlock", wholeVertexBlock);
  auto fetchStartBlock = createBlock(".fetchStart", fetchUberStartBlock);

  unsigned location = inst->getLocation();
  auto zero = builder.getInt32(0);
  builder.SetInsertPoint(currentBlock->getTerminator());
  builder.CreateBr(fetchStartBlock);
  currentBlock->getTerminator()->eraseFromParent();
  builder.SetInsertPoint(fetchStartBlock);

  auto locationAnd = builder.CreateAnd(locMasks, builder.getInt64(1ull << location));
  auto isAttriValid = builder.CreateICmpNE(locationAnd, builder.getInt64(0));
  builder.CreateCondBr(isAttriValid, fetchUberStartBlock, fetchEndBlock);

  builder.SetInsertPoint(fetchUberStartBlock);
  // The size of each input descriptor is sizeof(UberFetchShaderAttribInfo). vector4
  auto uberFetchAttrType = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
  descPtr = builder.CreateGEP(uberFetchAttrType, descPtr, {builder.getInt32(location)});
  auto uberFetchAttr = builder.CreateLoad(vbDescTy, descPtr);

  // The first DWord
  auto attr = builder.CreateExtractElement(uberFetchAttr, uint64_t(0));

  // The second DWord
  auto byteOffset = builder.CreateExtractElement(uberFetchAttr, 1);

  // The third DWord
  auto inputRate = builder.CreateExtractElement(uberFetchAttr, 2);

  // The fourth DWord
  auto bufferFormat = builder.CreateExtractElement(uberFetchAttr, 3);

  // attr[0~7]
  auto descBinding = builder.CreateAnd(attr, builder.getInt32(0xFF));

  // attr[8]
  auto perInstance = builder.CreateAnd(attr, builder.getInt32(0x100));

  // attr[10]
  auto isPacked = builder.CreateAnd(attr, builder.getInt32(0x400));
  isPacked = builder.CreateICmpNE(isPacked, zero);

  // attr[12~15]
  auto componentSize = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                               {attr, builder.getInt32(12), builder.getInt32(4)});

  auto xMask = builder.CreateAnd(attr, builder.getInt32(0x10000u));
  auto yMask = builder.CreateAnd(attr, builder.getInt32(0x20000u));
  auto zMask = builder.CreateAnd(attr, builder.getInt32(0x40000u));
  auto wMask = builder.CreateAnd(attr, builder.getInt32(0x80000u));
  xMask = builder.CreateICmpNE(xMask, zero);
  yMask = builder.CreateICmpNE(yMask, zero);
  zMask = builder.CreateICmpNE(zMask, zero);
  wMask = builder.CreateICmpNE(wMask, zero);

  // attr[20]
  auto isBgr = builder.CreateAnd(attr, builder.getInt32(0x0100000));
  isBgr = builder.CreateICmpNE(isBgr, zero);

  // Load VbDesc
  Value *vbDescPtr = builder.CreateGEP(vbDescTy, m_vertexBufTablePtr, descBinding);
  LoadInst *loadInst = builder.CreateLoad(vbDescTy, vbDescPtr);
  loadInst->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(loadInst->getContext(), {}));
  loadInst->setAlignment(Align(16));
  Value *vbDesc = loadInst;

  auto isPerInstance = builder.CreateICmpNE(perInstance, zero);

  // PerInstance
  auto vbIndexInstance = ShaderInputs::getInput(ShaderInput::InstanceId, builder, *m_lgcContext);

  // NOTE: When inputRate = 0, avoid adding new blocks and use two select instead.
  auto isZero = builder.CreateICmpEQ(inputRate, zero);
  auto divisor = builder.CreateSelect(isZero, builder.getInt32(1), inputRate);
  vbIndexInstance = builder.CreateUDiv(vbIndexInstance, divisor);
  vbIndexInstance = builder.CreateSelect(isZero, zero, vbIndexInstance);

  vbIndexInstance =
      builder.CreateAdd(vbIndexInstance, ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder));

  // Select VbIndex
  Value *vbIndex = builder.CreateSelect(isPerInstance, vbIndexInstance, m_vertexIndex);

  const auto inputTy = inst->getType();
  Type *basicTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;
  const unsigned bitWidth = basicTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  Intrinsic::ID instId = Intrinsic::amdgcn_struct_buffer_load_format;
  if (m_useSoftwareVertexBufferDescriptors) {
    instId = Intrinsic::amdgcn_raw_buffer_load_format;
    auto srdStride = builder.CreateExtractElement(vbDesc, 3);
    byteOffset = builder.CreateAdd(builder.CreateMul(vbIndex, srdStride), byteOffset);
  }
  // Replace buffer format
  vbDesc = builder.CreateInsertElement(vbDesc, bufferFormat, 3);

  SmallVector<Value *, 5> args;
  args.push_back(vbDesc);
  if (!m_useSoftwareVertexBufferDescriptors)
    args.push_back(vbIndex);
  unsigned offsetIdx = args.size();
  args.push_back(byteOffset);
  args.push_back(builder.getInt32(0));
  args.push_back(builder.getInt32(0));

  // If ispacked is false, we require per-component fetch
  builder.CreateCondBr(isPacked, wholeVertexBlock, comp0Block);

  // 8-bit vertex fetches use the 16-bit path as well.
  bool is16bitFetch = bitWidth <= 16;
  bool is64bitFetch = bitWidth == 64;
  auto fetch64Type = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
  auto fetch32Type = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
  auto fetch16Type = FixedVectorType::get(Type::getInt16Ty(*m_context), 4);
  auto fetchType = is16bitFetch ? fetch16Type : fetch32Type;

  Value *wholeVertex = nullptr;
  {
    builder.SetInsertPoint(wholeVertexBlock);
    wholeVertex = builder.CreateIntrinsic(instId, fetchType, args, {});
    if (is64bitFetch) {
      // If it is 64-bit, we need the second fetch
      args[offsetIdx] = builder.CreateAdd(args[offsetIdx], builder.getInt32(SizeOfVec4));
      auto secondFetch = builder.CreateIntrinsic(instId, fetchType, args, {});
      wholeVertex = builder.CreateShuffleVector(wholeVertex, secondFetch, ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7});
    }
    builder.CreateBr(fetchUberEndBlock);
  }

  fetchType = FixedVectorType::get(builder.getInt32Ty(), is64bitFetch ? 8 : 4);
  if (is16bitFetch)
    fetchType = FixedVectorType::get(builder.getInt16Ty(), 4);

  // Initialize the default values
  Value *lastVert = nullptr;
  if (basicTy->isIntegerTy()) {
    if (bitWidth <= 16)
      lastVert = m_fetchDefaults.int16;
    else if (bitWidth == 32)
      lastVert = m_fetchDefaults.int32;
    else {
      assert(bitWidth == 64);
      lastVert = m_fetchDefaults.int64;
    }
  } else if (basicTy->isFloatingPointTy()) {
    if (bitWidth == 16)
      lastVert = m_fetchDefaults.float16;
    else if (bitWidth == 32)
      lastVert = m_fetchDefaults.float32;
    else {
      assert(bitWidth == 64);
      lastVert = m_fetchDefaults.double64;
    }
  } else
    llvm_unreachable("Should never be called!");

  Value *comp0 = nullptr;
  Value *comp1 = nullptr;
  Value *comp2 = nullptr;
  Value *comp3 = nullptr;
  auto compType = is16bitFetch ? builder.getInt16Ty() : builder.getInt32Ty();

  // Per-component fetch
  // X channel
  // .comp0Block
  {
    builder.SetInsertPoint(comp0Block);

    args[offsetIdx] = byteOffset;
    if (is64bitFetch) {
      Value *comp = builder.CreateIntrinsic(instId, fetch64Type, args, {});
      Value *elem = builder.CreateExtractElement(comp, uint64_t(0));
      lastVert = builder.CreateInsertElement(lastVert, elem, uint64_t(0));
      elem = builder.CreateExtractElement(comp, 1);
      lastVert = builder.CreateInsertElement(lastVert, elem, 1);
      comp0 = lastVert;
    } else {
      comp0 = builder.CreateIntrinsic(instId, compType, args, {});
      lastVert = builder.CreateInsertElement(lastVert, comp0, uint64_t(0));
      comp0 = lastVert;
    }
    // If Y channel is 0, we will fetch the second component.
    builder.CreateCondBr(yMask, comp1Block, perCompEndBlock);
  }

  // Y channel
  // .comp1Block
  {
    builder.SetInsertPoint(comp1Block);
    // Add offset. offset = offset + componentSize
    args[offsetIdx] = builder.CreateAdd(args[offsetIdx], componentSize);
    if (is64bitFetch) {
      Value *comp = builder.CreateIntrinsic(instId, fetch64Type, args, {});
      Value *elem = builder.CreateExtractElement(comp, uint64_t(0));
      lastVert = builder.CreateInsertElement(lastVert, elem, 2);
      elem = builder.CreateExtractElement(comp, 1);
      lastVert = builder.CreateInsertElement(lastVert, elem, 3);
      comp1 = lastVert;
    } else {
      comp1 = builder.CreateIntrinsic(instId, compType, args, {});
      lastVert = builder.CreateInsertElement(lastVert, comp1, 1);
      comp1 = lastVert;
    }
    builder.CreateCondBr(zMask, comp2Block, perCompEndBlock);
  }

  // Z channel
  // .comp2Block
  {
    builder.SetInsertPoint(comp2Block);
    args[offsetIdx] = builder.CreateAdd(args[offsetIdx], componentSize);
    if (is64bitFetch) {
      Value *comp = builder.CreateIntrinsic(instId, fetch64Type, args, {});
      Value *elem = builder.CreateExtractElement(comp, uint64_t(0));
      lastVert = builder.CreateInsertElement(lastVert, elem, 4);
      elem = builder.CreateExtractElement(comp, 1);
      lastVert = builder.CreateInsertElement(lastVert, elem, 5);
      comp2 = lastVert;
    } else {
      comp2 = builder.CreateIntrinsic(instId, compType, args, {});
      lastVert = builder.CreateInsertElement(lastVert, comp2, 2);
      comp2 = lastVert;
    }
    builder.CreateCondBr(wMask, comp3Block, perCompEndBlock);
  }

  // W channel
  // .comp3Block
  {
    builder.SetInsertPoint(comp3Block);
    args[offsetIdx] = builder.CreateAdd(args[offsetIdx], componentSize);
    if (is64bitFetch) {
      Value *comp = builder.CreateIntrinsic(instId, fetch64Type, args, {});
      Value *elem = builder.CreateExtractElement(comp, uint64_t(0));
      lastVert = builder.CreateInsertElement(lastVert, elem, 6);
      elem = builder.CreateExtractElement(comp, 1);
      lastVert = builder.CreateInsertElement(lastVert, elem, 7);
      comp3 = lastVert;
    } else {
      comp3 = builder.CreateIntrinsic(instId, compType, args, {});
      lastVert = builder.CreateInsertElement(lastVert, comp3, 3);
      comp3 = lastVert;
    }
    builder.CreateBr(perCompEndBlock);
  }

  // .perCompEnd
  {
    builder.SetInsertPoint(perCompEndBlock);
    auto phiInst = builder.CreatePHI(lastVert->getType(), 4);
    phiInst->addIncoming(comp0, comp0Block);
    phiInst->addIncoming(comp1, comp1Block);
    phiInst->addIncoming(comp2, comp2Block);
    phiInst->addIncoming(comp3, comp3Block);
    lastVert = phiInst;
    // If the format is bgr, fix the order. It only is included in 32-bit format.
    if (!is64bitFetch) {
      auto fixedVertex = builder.CreateShuffleVector(lastVert, lastVert, ArrayRef<int>{2, 1, 0, 3});
      lastVert = builder.CreateSelect(isBgr, fixedVertex, lastVert);
    }
    builder.CreateBr(fetchUberEndBlock);
  }

  // .fetchUberEndBlock
  builder.SetInsertPoint(fetchUberEndBlock);
  auto phiInst = builder.CreatePHI(lastVert->getType(), 2);
  phiInst->addIncoming(wholeVertex, wholeVertexBlock);
  phiInst->addIncoming(lastVert, perCompEndBlock);
  lastVert = phiInst;

  // Get vertex fetch values
  const unsigned fetchCompCount = cast<FixedVectorType>(lastVert->getType())->getNumElements();
  std::vector<Value *> fetchValues(fetchCompCount);

  for (unsigned i = 0; i < fetchCompCount; ++i) {
    fetchValues[i] = builder.CreateExtractElement(lastVert, ConstantInt::get(Type::getInt32Ty(*m_context), i));
  }

  // Construct vertex fetch results
  const unsigned inputCompCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned vertexCompCount = inputCompCount * (bitWidth == 64 ? 2 : 1);

  std::vector<Value *> vertexValues(vertexCompCount);

  // NOTE: Original component index is based on the basic scalar type.
  unsigned compIdx = cast<ConstantInt>(inst->getElemIdx())->getZExtValue();
  compIdx *= (bitWidth == 64 ? 2 : 1);

  // Vertex input might take values from vertex fetch values or default fetch values
  for (unsigned i = 0; i < vertexCompCount; i++) {
    if (compIdx + i < fetchCompCount)
      vertexValues[i] = fetchValues[compIdx + i];
    else {
      llvm_unreachable("Should never be called!");
      vertexValues[i] = PoisonValue::get(Type::getInt32Ty(*m_context));
    }
  }

  Value *vertex = nullptr;
  if (vertexCompCount == 1)
    vertex = vertexValues[0];
  else {
    auto compType = bitWidth <= 16 ? Type::getInt16Ty(*m_context) : Type::getInt32Ty(*m_context);
    Type *vertexTy = FixedVectorType::get(compType, vertexCompCount);
    vertex = PoisonValue::get(vertexTy);

    for (unsigned i = 0; i < vertexCompCount; ++i)
      vertex = builder.CreateInsertElement(vertex, vertexValues[i], ConstantInt::get(Type::getInt32Ty(*m_context), i));
  }

  const bool is8bitFetch = (inputTy->getScalarSizeInBits() == 8);

  if (is8bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i16> now. For 8-bit vertex fetch, we have to
    // convert them to <n x i8> and the 8 high bits is truncated.
    assert(inputTy->isIntOrIntVectorTy()); // Must be integer type

    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt8Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(FixedVectorType::get(truncTy, cast<FixedVectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = builder.CreateTrunc(vertex, truncTy);
  }

  if (vertex->getType() != inputTy)
    vertex = builder.CreateBitCast(vertex, inputTy);
  builder.CreateBr(fetchEndBlock);

  // .fetchEndBlock
  {
    builder.SetInsertPoint(&*fetchEndBlock->getFirstInsertionPt());
    auto phiInst = builder.CreatePHI(inputTy, 2);
    phiInst->addIncoming(PoisonValue::get(inputTy), fetchStartBlock);
    phiInst->addIncoming(vertex, fetchUberEndBlock);
    vertex = phiInst;
    vertex->setName("vertex");
  }
  return vertex;
}

// =====================================================================================================================
// Create a VertexFetch
VertexFetch *VertexFetch::create(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors) {
  return new VertexFetchImpl(lgcContext, useSoftwareVertexBufferDescriptors);
}

// =====================================================================================================================
// Constructor
//
// @param context : LLVM context
VertexFetchImpl::VertexFetchImpl(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors)
    : m_lgcContext(lgcContext), m_context(&lgcContext->getContext()),
      m_useSoftwareVertexBufferDescriptors(useSoftwareVertexBufferDescriptors) {

  // Initialize default fetch values
  auto zero = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
  auto one = ConstantInt::get(Type::getInt32Ty(*m_context), 1);
  auto int16Zero = ConstantInt::get(Type::getInt16Ty(*m_context), 0);
  auto int16One = ConstantInt::get(Type::getInt16Ty(*m_context), 1);

  // Int16 (0, 0, 0, 1)
  m_fetchDefaults.int16 = ConstantVector::get({int16Zero, int16Zero, int16Zero, int16One});

  // Int (0, 0, 0, 1)
  m_fetchDefaults.int32 = ConstantVector::get({zero, zero, zero, one});

  // Int64 (0, 0, 0, 1)
  m_fetchDefaults.int64 = ConstantVector::get({zero, zero, zero, zero, zero, zero, zero, one});

  // Float16 (0, 0, 0, 1.0)
  const uint16_t float16One = 0x3C00;
  auto float16OneVal = ConstantInt::get(Type::getInt16Ty(*m_context), float16One);
  m_fetchDefaults.float16 = ConstantVector::get({int16Zero, int16Zero, int16Zero, float16OneVal});

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
                                    unsigned compIdx, BuilderImpl &builderImpl) {
  Value *vertex = nullptr;
  BuilderBase &builder = BuilderBase::get(builderImpl);
  Instruction *insertPos = &*builder.GetInsertPoint();
  auto vbDesc = loadVertexBufferDescriptor(description->binding, builderImpl);
  Value *srdStride = nullptr;
  if (m_useSoftwareVertexBufferDescriptors)
    std::tie(vbDesc, srdStride) = convertSrdToOffsetMode(vbDesc, builder);

  Value *vbIndex = nullptr;
  if (description->inputRate == VertexInputRateVertex) {
    // Use vertex index
    if (!m_vertexIndex) {
      IRBuilder<>::InsertPointGuard guard(builder);
      builder.SetInsertPointPastAllocas(insertPos->getFunction());
      m_vertexIndex = ShaderInputs::getVertexIndex(builder, *m_lgcContext);
    }
    vbIndex = m_vertexIndex;
  } else {
    if (description->divisor == 0) {
      vbIndex = ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder);
    } else if (description->divisor == 1) {
      // Use instance index
      if (!m_instanceIndex) {
        IRBuilder<>::InsertPointGuard guard(builder);
        builder.SetInsertPointPastAllocas(insertPos->getFunction());
        m_instanceIndex = ShaderInputs::getInstanceIndex(builder, *m_lgcContext);
      }
      vbIndex = m_instanceIndex;
    } else {
      // There is a divisor.
      vbIndex = builder.CreateUDiv(ShaderInputs::getInput(ShaderInput::InstanceId, builder, *m_lgcContext),
                                   builder.getInt32(description->divisor));
      vbIndex = builder.CreateAdd(vbIndex, ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder));
    }
  }

  Value *vertexFetches[2] = {}; // Two vertex fetch operations might be required
  Value *vertexFetch = nullptr; // Coalesced vector by combining the results of two vertex fetch operations

  VertexFormatInfo formatInfo = getVertexFormatInfo(description);

  const bool is16bitFetch = (inputTy->getScalarSizeInBits() <= 16);

  // Do the first vertex fetch operation
  addVertexFetchInst(vbDesc, formatInfo.numChannels, is16bitFetch, vbIndex, srdStride, description->offset,
                     description->stride, formatInfo.dfmt, formatInfo.nfmt, insertPos, &vertexFetches[0]);

  // Do post-processing in certain cases
  std::vector<Constant *> shuffleMask;
  bool postShuffle = needPostShuffle(description, shuffleMask);
  bool patch32 = needPatch32(description);
  if (postShuffle || patch32) {
    if (postShuffle) {
      // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
      // get the components in the right order.
      assert(shuffleMask.empty() == false);
      vertexFetches[0] =
          new ShuffleVectorInst(vertexFetches[0], vertexFetches[0], ConstantVector::get(shuffleMask), "", insertPos);
    }

    if (patch32) {
      bool isSigned = (description->nfmt == BufNumFormatSscaled || description->nfmt == BufNumFormatSnorm ||
                       description->nfmt == BufNumFormatFixed);

      // Whether need to do normalization emulation.
      bool isNorm = (description->nfmt == BufNumFormatSnorm || description->nfmt == BufNumFormatUnorm);

      // Whether need to do fixed point emulation
      bool isFixed = (description->nfmt == BufNumFormatFixed);

      // Whether need to translate from int bits to float bits.
      bool needTransToFp = (description->nfmt == BufNumFormatSscaled || description->nfmt == BufNumFormatSnorm ||
                            description->nfmt == BufNumFormatUscaled || description->nfmt == BufNumFormatUnorm);

      // Only for 32 bits format patch and emulation.
      for (unsigned i = 0; i < formatInfo.numChannels; ++i) {
        Value *elemInstr = ExtractElementInst::Create(vertexFetches[0],
                                                      ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
        if (needTransToFp) {
          // A constant divisor for normalization emulation.
          float normDiv = 2.14748365e+09f;
          if (isSigned) {
            // Signed int to float
            elemInstr = new SIToFPInst(elemInstr, Type::getFloatTy(*m_context), "", insertPos);
          } else {
            // Unsigned int to float
            elemInstr = new UIToFPInst(elemInstr, Type::getFloatTy(*m_context), "", insertPos);
            normDiv = 4.29496730e+09f;
          }
          if (isNorm) {
            // Normalization emulation.
            elemInstr = BinaryOperator::CreateFDiv(elemInstr, ConstantFP::get(Type::getFloatTy(*m_context), normDiv),
                                                   "", insertPos);
          }
        } else if (isFixed) {
          // A constant divisor to translate loaded float bits to fixed point format.
          float fixedPointMul = 1.0f / 65536.0f;
          elemInstr = new SIToFPInst(elemInstr, Type::getFloatTy(*m_context), "", insertPos);
          elemInstr = BinaryOperator::CreateFMul(
              elemInstr, ConstantFP::get(Type::getFloatTy(*m_context), fixedPointMul), "", insertPos);
        } else {
          llvm_unreachable("Should never be called!");
        }

        elemInstr = new BitCastInst(elemInstr, Type::getInt32Ty(*m_context), "", insertPos);
        vertexFetches[0] = InsertElementInst::Create(vertexFetches[0], elemInstr,
                                                     ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
      }
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

    addVertexFetchInst(vbDesc, numChannels, is16bitFetch, vbIndex, srdStride, description->offset + SizeOfVec4,
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
          PoisonValue::get(Type::getInt32Ty(*m_context)), PoisonValue::get(Type::getInt32Ty(*m_context))};
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
  bool needDoubleEmulation =
      description->dfmt >= BufDataFormat64 && description->dfmt <= BufDataFormat64_64_64_64 && basicTy->isFloatTy();
  if (needDoubleEmulation)
    basicTy = Type::getDoubleTy(*m_context);
  const unsigned bitWidth = basicTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  // Get default fetch values
  Constant *defaults = nullptr;

  if (basicTy->isIntegerTy()) {
    if (bitWidth <= 16)
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
      vertexValues[i] = PoisonValue::get(Type::getInt32Ty(*m_context));
    }
  }

  if (vertexCompCount == 1)
    vertex = vertexValues[0];
  else {
    Type *vertexTy = is16bitFetch ? FixedVectorType::get(Type::getInt16Ty(*m_context), vertexCompCount)
                                  : FixedVectorType::get(Type::getInt32Ty(*m_context), vertexCompCount);
    vertex = PoisonValue::get(vertexTy);

    for (unsigned i = 0; i < vertexCompCount; ++i) {
      vertex = InsertElementInst::Create(vertex, vertexValues[i], ConstantInt::get(Type::getInt32Ty(*m_context), i), "",
                                         insertPos);
    }
  }

  const bool is8bitFetch = (inputTy->getScalarSizeInBits() == 8);
  if (is8bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i16> now. For 8-bit vertex fetch, we have to
    // convert them to <n x i8> and the 8 high bits is truncated.
    assert(inputTy->isIntOrIntVectorTy()); // Must be integer type

    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt8Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(FixedVectorType::get(truncTy, cast<FixedVectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = new TruncInst(vertex, truncTy, "", insertPos);
  }

  if (needDoubleEmulation) {
    // SPIR-V extended format emulation
    // If input type is float32 but vertex attribute data format is float64, we need another float point trunc step.
    int vecSize = cast<FixedVectorType>(vertex->getType())->getNumElements() / 2;
    vertex = new BitCastInst(vertex, FixedVectorType::get(Type::getDoubleTy(*m_context), vecSize), "", insertPos);
    vertex = new FPTruncInst(vertex, FixedVectorType::get(Type::getFloatTy(*m_context), vecSize), "", insertPos);
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
  case BufDataFormat8_8_8:
    info.dfmt = BufDataFormat8_8_8;
    info.numChannels = 3;
    break;
  case BufDataFormat16_16_16:
    info.dfmt = BufDataFormat16_16_16;
    info.numChannels = 3;
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
  assert(nfmt < 9);
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
  case 11:
    assert(dfmt < sizeof(m_vertexFormatMapGfx11) / sizeof(m_vertexFormatMapGfx11[0]));
    assert(nfmt < sizeof(m_vertexFormatMapGfx11[0]) / sizeof(m_vertexFormatMapGfx11[0][0]));
    format = m_vertexFormatMapGfx11[dfmt][nfmt];
    break;
  }
  return format;
}

// =====================================================================================================================
// Loads vertex descriptor based on the specified vertex input location.
//
// @param binding : ID of vertex buffer binding
// @param builder : Builder with insert point set
Value *VertexFetchImpl::loadVertexBufferDescriptor(unsigned binding, BuilderImpl &builderImpl) {
  BuilderBase &builder = BuilderBase::get(builderImpl);
  if (builderImpl.useVertexBufferDescArray()) {
    Value *vtxDesc = nullptr;
    // Create descriptor for current attribute
    if (binding == VertexInputBindingCurrent) {
      if (m_curAttribBufferDescr == nullptr) {
        IRBuilder<>::InsertPointGuard guard(builder);
        builder.SetInsertPointPastAllocas(builder.GetInsertBlock()->getParent());
        auto descPtr = builderImpl.CreateBufferDesc(InternalDescriptorSetId, CurrentAttributeBufferBinding,
                                                    builderImpl.getInt32(0), lgc::Builder::BufferFlagAddress);
        // Create descriptor by a 64-bits pointer
        m_curAttribBufferDescr = builderImpl.buildInlineBufferDesc(descPtr);
      }
      vtxDesc = m_curAttribBufferDescr;
    } else {
      // Create descriptor for vertex buffer
      vtxDesc = builderImpl.CreateBufferDesc(InternalDescriptorSetId, GenericVertexFetchShaderBinding,
                                             builderImpl.getInt32(binding), lgc::Builder::BufferFlagNonConst);
    }

    return vtxDesc;
  }

  // Get the vertex buffer table pointer as pointer to v4i32 descriptor.
  Type *vbDescTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
  if (!m_vertexBufTablePtr) {
    IRBuilder<>::InsertPointGuard guard(builder);
    builder.SetInsertPointPastAllocas(builder.GetInsertPoint()->getFunction());
    m_vertexBufTablePtr =
        ShaderInputs::getSpecialUserDataAsPointer(UserDataMapping::VertexBufferTable, vbDescTy, builder);
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
// @param srdStride: Stride from SRD. Only for offset mode.
// @param stride : Vertex attribute stride (in bytes)
// @param dfmt : Date format of vertex buffer
// @param nfmt : Numeric format of vertex buffer
// @param insertPos : Where to insert instructions
// @param [out] ppFetch : Destination of vertex fetch
void VertexFetchImpl::addVertexFetchInst(Value *vbDesc, unsigned numChannels, bool is16bitFetch, Value *vbIndex,
                                         Value *srdStride, unsigned offset, unsigned stride, unsigned dfmt,
                                         unsigned nfmt, Instruction *insertPos, Value **ppFetch) const {

  const VertexCompFormatInfo *formatInfo = getVertexComponentFormatInfo(dfmt);

  Intrinsic::ID instId = Intrinsic::amdgcn_struct_tbuffer_load;
  BuilderBase builder(insertPos);
  Value *instOffset = builder.getInt32(offset);
  if (m_useSoftwareVertexBufferDescriptors) {
    instId = Intrinsic::amdgcn_raw_tbuffer_load;
    auto index2Offset = builder.CreateMul(vbIndex, srdStride);
    instOffset = builder.CreateAdd(index2Offset, instOffset);
  }

  // NOTE: If the vertex attribute offset and stride are aligned on data format boundaries, we can do a vertex fetch
  // operation to read the whole vertex. Otherwise, we have to do vertex per-component fetch operations.
  if (((offset % formatInfo->vertexByteSize) == 0 && (stride % formatInfo->vertexByteSize) == 0 &&
       // NOTE: For the vertex data format 8_8, 8_8_8_8, 16_16, and 16_16_16_16, tbuffer_load has a HW defect when
       // vertex buffer is unaligned. Therefore, we have to split the vertex fetch to component-based ones
       dfmt != BufDataFormat8_8 && dfmt != BufDataFormat8_8_8_8 && dfmt != BufDataFormat16_16 &&
       dfmt != BufDataFormat16_16_16_16 && dfmt != BufDataFormat8_8_8 && dfmt != BufDataFormat16_16_16) ||
      formatInfo->compDfmt == dfmt) {

    SmallVector<Value *, 6> args;
    args.push_back(vbDesc);
    if (!m_useSoftwareVertexBufferDescriptors)
      args.push_back(vbIndex);
    args.push_back(instOffset);
    args.push_back(builder.getInt32(0));
    args.push_back(builder.getInt32(mapVertexFormat(dfmt, nfmt)));
    args.push_back(builder.getInt32(0));

    // Do vertex fetch
    Type *fetchTy = nullptr;

    if (is16bitFetch) {
      switch (numChannels) {
      case 1:
        fetchTy = Type::getHalfTy(*m_context);
        break;
      case 2:
        fetchTy = FixedVectorType::get(Type::getHalfTy(*m_context), 2);
        break;
      case 3:
      case 4:
        fetchTy = FixedVectorType::get(Type::getHalfTy(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    } else {
      switch (numChannels) {
      case 1:
        fetchTy = Type::getInt32Ty(*m_context);
        break;
      case 2:
        fetchTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
        break;
      case 3:
      case 4:
        fetchTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    }

    Value *fetch = builder.CreateIntrinsic(instId, fetchTy, args, {});

    if (is16bitFetch) {
      // NOTE: The fetch values are represented by <n x i16>, so we will bitcast the float16 values to
      // int16 eventually.
      Type *bitCastTy = Type::getInt16Ty(*m_context);
      bitCastTy = numChannels == 1 ? bitCastTy : FixedVectorType::get(bitCastTy, numChannels >= 3 ? 4 : numChannels);
      fetch = new BitCastInst(fetch, bitCastTy, "", insertPos);
    }

    if (numChannels == 3) {
      // NOTE: If valid number of channels is 3, the actual fetch type should be revised from <4 x i32>
      // to <3 x i32>.
      *ppFetch = new ShuffleVectorInst(fetch, fetch, ArrayRef<int>{0, 1, 2}, "", insertPos);
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
      compVbIndices[i] = vbIndex;
      compOffsets[i] = compOffset;
    }

    Type *fetchTy = is16bitFetch ? FixedVectorType::get(Type::getInt16Ty(*m_context), numChannels)
                                 : FixedVectorType::get(Type::getInt32Ty(*m_context), numChannels);
    Value *fetch = PoisonValue::get(fetchTy);

    SmallVector<Value *, 6> args;
    args.push_back(vbDesc);
    if (!m_useSoftwareVertexBufferDescriptors)
      args.push_back(vbIndex);
    unsigned offsetIdx = args.size();
    args.push_back(instOffset);
    args.push_back(builder.getInt32(0));
    args.push_back(builder.getInt32(mapVertexFormat(formatInfo->compDfmt, nfmt)));
    args.push_back(builder.getInt32(0));

    // Do vertex per-component fetches
    for (unsigned i = 0; i < formatInfo->compCount; ++i) {
      Value *compOffset = builder.getInt32(compOffsets[i]);
      if (m_useSoftwareVertexBufferDescriptors)
        args[offsetIdx] = builder.CreateAdd(instOffset, compOffset);
      else
        args[offsetIdx] = compOffset;

      Value *compFetch = nullptr;
      if (is16bitFetch) {
        compFetch = builder.CreateIntrinsic(instId, builder.getHalfTy(), args, {});
        compFetch = builder.CreateBitCast(compFetch, builder.getInt16Ty());
      } else {
        compFetch = builder.CreateIntrinsic(instId, builder.getInt32Ty(), args, {});
      }

      fetch = builder.CreateInsertElement(fetch, compFetch, i);
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
// Checks whether a patch (emulation) step is needed for some 32 bits vertex attribute formats.
//
// @param inputDesc : Vertex input description
bool VertexFetchImpl::needPatch32(const VertexInputDescription *inputDesc) const {
  bool needPatch = false;

  switch (inputDesc->dfmt) {
  case BufDataFormat32:
  case BufDataFormat32_32:
  case BufDataFormat32_32_32:
  case BufDataFormat32_32_32_32:
    if (inputDesc->nfmt == BufNumFormatSscaled || inputDesc->nfmt == BufNumFormatUscaled ||
        inputDesc->nfmt == BufNumFormatSnorm || inputDesc->nfmt == BufNumFormatUnorm ||
        inputDesc->nfmt == BufNumFormatFixed)
      needPatch = true;
    break;
  default:
    break;
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
// Convert D3D12_VERTEX_BUFFER_VIEW SRD to offset mode. Stride will be used to calculate offset.
//
// @param vbDesc : Original SRD
// @param builder : Builder to use to insert vertex fetch instructions
// @returns : {New SRD,stride}
std::pair<Value *, Value *> VertexFetchImpl::convertSrdToOffsetMode(Value *vbDesc, BuilderBase &builder) {
  assert(m_useSoftwareVertexBufferDescriptors);
  // NOTE: Vertex buffer SRD is D3D12_VERTEX_BUFFER_VIEW
  // struct VertexBufferView
  // {
  //   gpusize gpuva;
  //   uint32  sizeInBytes;
  //   uint32  strideInBytes;
  // };

  // Stride is from the third DWORD.
  auto srdStride = builder.CreateExtractElement(vbDesc, 3);

  SqBufRsrcWord3 sqBufRsrcWord3;
  sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
  sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
  sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
  sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
  GfxIpVersion gfxIp = m_lgcContext->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major == 10) {
    sqBufRsrcWord3.gfx10.format = BUF_FORMAT_32_UINT;
    sqBufRsrcWord3.gfx10.resourceLevel = 1;
    sqBufRsrcWord3.gfx10.oobSelect = 3;
  } else if (gfxIp.major >= 11) {
    sqBufRsrcWord3.gfx11.format = BUF_FORMAT_32_UINT;
    sqBufRsrcWord3.gfx11.oobSelect = 3;
  }
  auto newDesc = builder.CreateInsertElement(vbDesc, builder.getInt32(sqBufRsrcWord3.u32All), 3);
  return {newDesc, srdStride};
}
