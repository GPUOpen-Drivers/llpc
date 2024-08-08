/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Represents vertex component info corresponding to vertex data format (BufDataFormat).
//
// NOTE: This info is used by vertex fetch instructions. We split vertex fetch into its per-component fetches when
// the original vertex fetch does not match the hardware requirements (such as vertex attribute offset, vertex
// attribute stride, etc..)
struct VertexCompFormatInfo {
  unsigned vertexByteSize; // Byte size of the vertex
  unsigned compByteSize;   // Byte size of each individual component
  unsigned compCount;      // Component count
  BufDataFmt fetchDfmt;    // Equivalent data format of each fetch intrinsic
};

// Represents vertex format data results info corresponding to vertex numerical format (BufNumFormat).
//
// This info will be used to determine property of each format, corresponding to final result emulation including
// packed formats and when do fetch in Byte.
struct VertexNumFormatInfo {
  bool isSigned; // Load result is signed, do SExt when needed.
  bool isScaled; // Load result is scaled.
  bool isNorm;   // Load result is normarlized.
};

// =====================================================================================================================
// Vertex fetch manager
class VertexFetchImpl : public VertexFetch {
public:
  VertexFetchImpl(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors, bool vbAddressLowBitsKnown);
  VertexFetchImpl(const VertexFetchImpl &) = delete;
  VertexFetchImpl &operator=(const VertexFetchImpl &) = delete;

  // Generate code to fetch a vertex value
  Value *fetchVertex(Type *inputTy, const VertexInputDescription *description, unsigned location, unsigned compIdx,
                     BuilderImpl &builderImpl) override;

  // Generate code to fetch a vertex value for uber shader
  Value *fetchVertex(InputImportGenericOp *inst, Value *descPtr, Value *locMasks, BuilderBase &builder,
                     bool disablePerCompFetch) override;

private:
  // Gets variable corresponding to vertex index
  Value *getVertexIndex() { return m_vertexIndex; }

  // Gets variable corresponding to instance index
  Value *getInstanceIndex() { return m_instanceIndex; }

  static const VertexCompFormatInfo *getVertexComponentFormatInfo(unsigned dfmt);

  static const VertexNumFormatInfo *getVertexNumericFormatInfo(unsigned nfmt);

  unsigned mapVertexFormat(unsigned dfmt, unsigned nfmt) const;

  Value *loadVertexBufferDescriptor(unsigned binding, BuilderImpl &builderImpl);

  void addVertexFetchInst(Value *vbDesc, Value *vbIndex, Value *srdStride, Type *inputTy, unsigned numChannels,
                          unsigned offset, unsigned dfmt, unsigned nfmt, unsigned inputCompBytes,
                          unsigned fetchCompBytes, bool isSigned, bool isPacked, bool fetchInByte,
                          BuilderImpl &builderImpl, Value **ppFetch) const;

  bool needPostShuffle(const VertexInputDescription *inputDesc, std::vector<Constant *> &shuffleMask) const;

  bool needPatch32(const VertexInputDescription *inputDesc) const;

  bool needPackFormatEmulation(const VertexInputDescription *inputDesc, const VertexNumFormatInfo *numFormatInfo,
                               std::vector<unsigned> &extractMask, std::vector<float> &normalizationFactors) const;

  void postFetchEmulation(const VertexInputDescription *description, bool fetchInByte, unsigned inputCompBytes,
                          unsigned numChannels, const VertexNumFormatInfo *numFormatInfo,
                          const VertexCompFormatInfo *descFormatInfo, BuilderImpl &builderImpl, Value **ppFetch) const;

  std::pair<Value *, Value *> convertSrdToOffsetMode(Value *vbDesc, BuilderImpl &builder);

  Type *getVertexFetchType(bool isFloat, unsigned byteSize, BuilderImpl &builderImpl) const;

  LgcContext *m_lgcContext = nullptr;      // LGC context
  LLVMContext *m_context = nullptr;        // LLVM context
  Value *m_vertexBufTablePtr = nullptr;    // Vertex buffer table pointer
  Value *m_curAttribBufferDescr = nullptr; // Current attribute buffer descriptor;
  Value *m_vertexIndex = nullptr;          // Vertex index
  Value *m_instanceIndex = nullptr;        // Instance index

  bool m_useSoftwareVertexBufferDescriptors = false; // Use software vertex buffer descriptors to structure SRD.
  bool m_vbAddressLowBitsKnown = false;              // Use vertex buffer offset low bits from driver.

  static const VertexCompFormatInfo m_vertexCompFormatInfo[]; // Info table of vertex component format
  static const VertexNumFormatInfo m_vertexNumFormatInfo[];   // Info table of vertex num format
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

// Initializes info table of vertex numerical format map
// <isSigned,  isScaled,  isNorm>
const VertexNumFormatInfo VertexFetchImpl::m_vertexNumFormatInfo[] = {
    {false, false, true},  // BUF_NUM_FORMAT_UNORM
    {true, false, true},   // BUF_NUM_FORMAT_SNORM
    {false, true, false},  // BUF_NUM_FORMAT_USCALED
    {true, true, false},   // BUF_NUM_FORMAT_SSCALED
    {false, false, false}, // BUF_NUM_FORMAT_UINT
    {true, false, false},  // BUF_NUM_FORMAT_SINT
    {true, false, false},  // BUF_NUM_FORMAT_SNORM_OGL
    {true, false, false},  // BUF_NUM_FORMAT_FLOAT
    {true, false, false},  // BUF_NUM_FORMAT_FIXED
};

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
    {4, 0, 3, BUF_DATA_FORMAT_10_11_11},   // BUF_DATA_FORMAT_10_11_11 (Packed)
    {4, 0, 3, BUF_DATA_FORMAT_11_11_10},   // BUF_DATA_FORMAT_11_11_10 (Packed)
    {4, 0, 4, BUF_DATA_FORMAT_10_10_10_2}, // BUF_DATA_FORMAT_10_10_10_2 (Packed)
    {4, 0, 4, BUF_DATA_FORMAT_2_10_10_10}, // BUF_DATA_FORMAT_2_10_10_10 (Packed)
    {4, 1, 4, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8_8_8_8
    {8, 4, 2, BUF_DATA_FORMAT_32},         // BUF_DATA_FORMAT_32_32
    {8, 2, 4, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16_16_16_16
    {12, 4, 3, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32
    {16, 4, 4, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32_32
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BufDataFormatReserved
    {4, 1, 4, BUF_DATA_FORMAT_8},          // BufDataFormat8_8_8_8_Bgra
    {3, 1, 3, BUF_DATA_FORMAT_8},          // BufDataFormat8_8_8
    {3, 0, 3, BUF_DATA_FORMAT_8},          // BufDataFormat8_8_8_Bgr,
    {4, 0, 4, BUF_DATA_FORMAT_2_10_10_10}, // BufDataFormat2_10_10_10_Bgra,
    {8, 8, 1, BUF_DATA_FORMAT_32},         // BufDataFormat64,
    {16, 8, 2, BUF_DATA_FORMAT_32},        // BufDataFormat64_64,
    {24, 8, 3, BUF_DATA_FORMAT_32},        // BufDataFormat64_64_64,
    {32, 8, 4, BUF_DATA_FORMAT_32},        // BufDataFormat64_64_64_64,
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

  // Gather vertex fetch calls. We can assume they're all in one function, the vertex shader.
  // We can assume that multiple fetches of the same location, component and type have been CSEd.
  SmallVector<InputImportGenericOp *, 8> vertexFetches;
  static const auto fetchVisitor = llvm_dialects::VisitorBuilder<SmallVectorImpl<InputImportGenericOp *>>()
                                       .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                       .add<InputImportGenericOp>([](auto &fetches, InputImportGenericOp &op) {
                                         if (lgc::getShaderStage(op.getFunction()) == ShaderStage::Vertex)
                                           fetches.push_back(&op);
                                       })
                                       .build();
  fetchVisitor.visit(vertexFetches, module);
  if (vertexFetches.empty())
    return PreservedAnalyses::all();

  std::unique_ptr<VertexFetch> vertexFetch(VertexFetch::create(
      pipelineState->getLgcContext(), pipelineState->getOptions().useSoftwareVertexBufferDescriptors,
      pipelineState->getOptions().vbAddressLowBitsKnown));
  BuilderImpl builder(pipelineState);

  if (pipelineState->getOptions().enableUberFetchShader) {
    // NOTE: The 10_10_10_2 formats are not supported by the uber fetch shader on gfx9 and older.
    // We rely on the driver to fallback to not using the uber fetch shader when those formats are used.
    builder.setShaderStage(ShaderStage::Vertex);
    builder.SetInsertPointPastAllocas(vertexFetches[0]->getFunction());
    auto desc = builder.CreateBufferDesc(InternalDescriptorSetId, FetchShaderInternalBufferBinding, builder.getInt32(0),
                                         Builder::BufferFlagAddress, false);

    auto descPtr = builder.CreateIntToPtr(desc, builder.getPtrTy(ADDR_SPACE_CONST));

    // 64 bit location masks.
    Value *locationMasks = builder.CreateLoad(builder.getInt64Ty(), descPtr);
    descPtr = builder.CreateGEP(builder.getInt64Ty(), descPtr, {builder.getInt32(1)});

    for (InputImportGenericOp *inst : vertexFetches) {
      builder.SetInsertPoint(inst);
      Value *vertex = vertexFetch->fetchVertex(inst, descPtr, locationMasks, BuilderBase::get(builder),
                                               pipelineState->getOptions().disablePerCompFetch);
      // Replace and erase this instruction.
      inst->replaceAllUsesWith(vertex);
      inst->eraseFromParent();
    }
    return PreservedAnalyses::none();
  }

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
      builder.setShaderStage(ShaderStage::Vertex);
      vertex = vertexFetch->fetchVertex(fetch->getType(), description, location, component, builder);
    }

    // Replace and erase this call.
    fetch->replaceAllUsesWith(vertex);
    fetch->eraseFromParent();
  }

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// This is an lgc.input.import.generic operation for vertex buffers.
// Executes vertex fetch operations based on the uber shader buffer.
//
// @param inst : the input instruction
// @param descPtr : 64bit address of buffer
// @param locMasks : determine if the attribute data is valid.
// @param builder : Builder to use to insert vertex fetch instructions
// @param disablePerCompFetch : disable per component fetch
// @returns : vertex
Value *VertexFetchImpl::fetchVertex(InputImportGenericOp *inst, Value *descPtr, Value *locMasks, BuilderBase &builder,
                                    bool disablePerCompFetch) {
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

  Type *vbDescTy = nullptr;
  { vbDescTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 4); }
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

  BasicBlock *fetchUberEndBlock = createBlock(".fetchUberEndBlock", fetchEndBlock);
  BasicBlock *perCompEndBlock = nullptr;
  BasicBlock *comp3Block = nullptr;
  BasicBlock *comp2Block = nullptr;
  BasicBlock *comp1Block = nullptr;
  BasicBlock *comp0Block = nullptr;
  if (!disablePerCompFetch) {
    perCompEndBlock = createBlock(".perCompEnd", fetchUberEndBlock);
    comp3Block = createBlock(".comp3Block", perCompEndBlock);
    comp2Block = createBlock(".comp2Block", comp3Block);
    comp1Block = createBlock(".comp1Block", comp2Block);
    comp0Block = createBlock(".comp0Block", comp1Block);
  }
  BasicBlock *wholeVertexBlock = createBlock(".wholeVertex", comp0Block);
  BasicBlock *fetchUberStartBlock = createBlock(".fetchUberStartBlock", wholeVertexBlock);
  BasicBlock *fetchStartBlock = createBlock(".fetchStart", fetchUberStartBlock);

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

  {
    if (m_useSoftwareVertexBufferDescriptors) {
      instId = Intrinsic::amdgcn_raw_buffer_load_format;
      auto srdStride = builder.CreateExtractElement(vbDesc, 3);
      byteOffset = builder.CreateAdd(builder.CreateMul(vbIndex, srdStride), byteOffset);
    }
    // Replace buffer format
    vbDesc = builder.CreateInsertElement(vbDesc, bufferFormat, 3);
  }

  SmallVector<Value *, 5> args;
  args.push_back(vbDesc);
  if (!m_useSoftwareVertexBufferDescriptors)
    args.push_back(vbIndex);
  unsigned offsetIdx = args.size();
  args.push_back(byteOffset);
  { args.push_back(builder.getInt32(0)); }
  args.push_back(builder.getInt32(0));

  if (disablePerCompFetch) {
    builder.CreateBr(wholeVertexBlock);
  } else {
    // If ispacked is false, we require per-component fetch
    builder.CreateCondBr(isPacked, wholeVertexBlock, comp0Block);
  }

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

  Value *lastVert = nullptr;
  if (!disablePerCompFetch) {
    fetchType = FixedVectorType::get(builder.getInt32Ty(), is64bitFetch ? 8 : 4);
    if (is16bitFetch)
      fetchType = FixedVectorType::get(builder.getInt16Ty(), 4);

    // Initialize the default values
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
  } else {
    // .fetchUberEndBlock
    builder.SetInsertPoint(fetchUberEndBlock);
    lastVert = wholeVertex;
  }

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
VertexFetch *VertexFetch::create(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors,
                                 bool vbAddressLowBitsKnown) {
  return new VertexFetchImpl(lgcContext, useSoftwareVertexBufferDescriptors, vbAddressLowBitsKnown);
}

// =====================================================================================================================
// Constructor
//
// @param context : LLVM context
VertexFetchImpl::VertexFetchImpl(LgcContext *lgcContext, bool useSoftwareVertexBufferDescriptors,
                                 bool vbAddressLowBitsKnown)
    : m_lgcContext(lgcContext), m_context(&lgcContext->getContext()),
      m_useSoftwareVertexBufferDescriptors(useSoftwareVertexBufferDescriptors),
      m_vbAddressLowBitsKnown(vbAddressLowBitsKnown) {

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
// Get vertex fetch related types referred to their type bit width. For 64 bit type, we do 32 bit fetch instead.
//
// @param isFloat : Whether target type is a float point type.
// @param byteSize : Byte (8 bit) size of target Type.
// @param builderImpl : BuilderImpl to use to insert vertex fetch instructions.
Type *VertexFetchImpl::getVertexFetchType(bool isFloat, unsigned byteSize, BuilderImpl &builderImpl) const {
  assert(byteSize == 1 || byteSize == 2 || byteSize == 4 || byteSize == 8);
  if (byteSize == 1) {
    assert(!isFloat);
    return builderImpl.getInt8Ty();
  } else if (byteSize == 2) {
    return isFloat ? builderImpl.getHalfTy() : builderImpl.getInt16Ty();
  } else {
    // HW doesn't support 64bit fetch intrinsics, hence we will use 32bit fetch for double times.
    return isFloat ? builderImpl.getFloatTy() : builderImpl.getInt32Ty();
  }
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
    std::tie(vbDesc, srdStride) = convertSrdToOffsetMode(vbDesc, builderImpl);
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

  Value *vertexFetch = nullptr;

  // Conponent format and numeric format info.
  const VertexCompFormatInfo *compFormatInfo = getVertexComponentFormatInfo(description->dfmt);
  const VertexNumFormatInfo *numFormatInfo = getVertexNumericFormatInfo(description->nfmt);

  // Input components' type
  Type *inputCompTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;
  unsigned inputCompBytes = inputCompTy->getScalarSizeInBits() / 8;

  // Location size of components. If its type is Double, each component consumes 2 locations.
  // For Double type, we still do 32 bit fetch.
  const unsigned compLocationSize = (std::max(inputCompBytes, compFormatInfo->compByteSize) + 3) / 4;
  compIdx *= compLocationSize;

  // Whether it is fetching with a packed format.
  bool isPacked = (compFormatInfo->compByteSize == 0);

  // If its a packed format, fetch in i32Vec4 type.
  uint32_t fetchCompBytes = isPacked ? 4 : compFormatInfo->compByteSize / compLocationSize;
  unsigned numChannels = compLocationSize * compFormatInfo->compCount;

  // Basically, do fetch in component, and in some special cases, we do fetch in Byte for alignment purpose.
  bool fetchInByte = false;
  if (m_vbAddressLowBitsKnown) {
    const uint32_t firstInstanceOffset = description->vbAddrLowBits + description->offset;
    const uint32_t alignStride = (description->stride == 0) ? fetchCompBytes : description->stride;
    fetchInByte =
        (firstInstanceOffset % fetchCompBytes != 0 || (firstInstanceOffset + alignStride) % fetchCompBytes != 0);
  }

  // After back-end optimization, intrinsics may be combined to fetch the whole vertex in generated ISA codes.
  // To make sure combination works, we need to keep tbuffer_load formats as same as possible when visit this function.
  // To avoid redundant extract and insert operation, we need to keep component bit width as same as input component.
  addVertexFetchInst(vbDesc, vbIndex, srdStride, inputCompTy, numChannels, description->offset,
                     compFormatInfo->fetchDfmt, description->nfmt, inputCompBytes, fetchCompBytes,
                     numFormatInfo->isSigned, isPacked, fetchInByte, builderImpl, &vertexFetch);

  // When do fetch in Byte, we need to emulate final results manually.
  postFetchEmulation(description, fetchInByte, inputCompBytes, numChannels, numFormatInfo, compFormatInfo, builderImpl,
                     &vertexFetch);

  // Finalize vertex fetch
  Type *vertexCompTy = getVertexFetchType(false, inputCompBytes, builderImpl);

  unsigned fetchCompCount =
      vertexFetch->getType()->isVectorTy() ? cast<FixedVectorType>(vertexFetch->getType())->getNumElements() : 1;
  const unsigned inputCompCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  unsigned vertexCompCount = inputCompCount * compLocationSize;
  unsigned insertCompCount = std::min(vertexCompCount, fetchCompCount - compIdx);

  // Final fetch results may be constructed by fetched values and default values.
  unsigned elemIdx = 0;
  vertex = PoisonValue::get(FixedVectorType::get(vertexCompTy, vertexCompCount));
  if (vertexCompCount == 1) {
    // If compIdx is larger than fetchCompCount, assign a default value to it later (depending on compIdx).
    if (compIdx < fetchCompCount) {
      vertex = fetchCompCount > 1 ? builderImpl.CreateExtractElement(vertexFetch, builderImpl.getInt32(compIdx))
                                  : vertexFetch;
      elemIdx++;
    }
  } else {
    if (numChannels > 1) {
      for (; elemIdx < insertCompCount; ++elemIdx) {
        Value *elemVal = builderImpl.CreateExtractElement(vertexFetch, builderImpl.getInt32(elemIdx + compIdx));
        vertex = builderImpl.CreateInsertElement(vertex, elemVal, builderImpl.getInt32(elemIdx));
      }
    } else {
      vertex = builderImpl.CreateInsertElement(vertex, vertexFetch, builderImpl.getInt32(elemIdx));
      elemIdx++;
    }
  }

  // Append default zero to unused channels.
  Value *defaultZero = ConstantInt::get(vertexCompTy, 0);
  for (; elemIdx < vertexCompCount - compLocationSize; ++elemIdx) {
    vertex = builderImpl.CreateInsertElement(vertex, defaultZero, builderImpl.getInt32(elemIdx));
  }

  if (compLocationSize == 2 && inputCompTy->isFloatTy()) {
    // SPIR-V extended format emulation
    // If input type is float32 but vertex attribute data format is float64, we need another float point trunc step.
    vertex = builderImpl.CreateBitCast(vertex, FixedVectorType::get(builderImpl.getDoubleTy(), vertexCompCount / 2));
    vertex = builderImpl.CreateFPTrunc(vertex, FixedVectorType::get(builderImpl.getFloatTy(), vertexCompCount / 2));
  }

  if (vertex->getType() != inputTy)
    vertex = builderImpl.CreateBitCast(vertex, inputTy);

  // Last default value may be zero or one, depending on component index and result channel number.
  if (elemIdx == vertexCompCount - compLocationSize) {
    elemIdx = elemIdx / compLocationSize;
    bool isOne = (elemIdx + compIdx == 3);
    Value *lastDefaultVal = nullptr;
    if (inputCompTy->isIntegerTy()) {
      lastDefaultVal = ConstantInt::get(inputCompTy, isOne ? 1 : 0);
    } else if (inputCompTy->isFloatingPointTy()) {
      lastDefaultVal = ConstantFP::get(inputCompTy, isOne ? 1.0 : 0);
    } else
      llvm_unreachable("Should never be called!");

    if (vertexCompCount > 1)
      vertex = builderImpl.CreateInsertElement(vertex, lastDefaultVal, builderImpl.getInt32(elemIdx));
    else
      vertex = lastDefaultVal;
  }

  vertex->setName("vertex" + Twine(location) + "." + Twine(compIdx));

  return vertex;
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
// Gets format property info from table according to vertex buffer num format.
//
// @param nfmt : Numeric format of vertex buffer
const VertexNumFormatInfo *VertexFetchImpl::getVertexNumericFormatInfo(unsigned nfmt) {
  assert(nfmt < sizeof(m_vertexNumFormatInfo) / sizeof(m_vertexNumFormatInfo[0]));
  return &m_vertexNumFormatInfo[nfmt];
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
  default:
    llvm_unreachable("unsupported GFX IP");
    break;
  }
  return format;
}

// =====================================================================================================================
// Checks whether bit extraction is required for packed format when do fetch in Byte.
//
// @param inputDesc : Vertex input description.
// @param [out] extractMask : Bits extract mask.
bool VertexFetchImpl::needPackFormatEmulation(const VertexInputDescription *inputDesc,
                                              const VertexNumFormatInfo *numFormatInfo,
                                              std::vector<unsigned> &extractMask,
                                              std::vector<float> &normalizationFactors) const {
  switch (inputDesc->dfmt) {
  case BufDataFormat10_11_11:
    extractMask.push_back(11);
    extractMask.push_back(10);
    extractMask.push_back(10);
    return true;
  case BufDataFormat11_11_10:
    extractMask.push_back(10);
    extractMask.push_back(11);
    extractMask.push_back(11);
    return true;
  case BufDataFormat10_10_10_2:
    extractMask.push_back(2);
    extractMask.push_back(10);
    extractMask.push_back(10);
    extractMask.push_back(10);
    if (numFormatInfo->isNorm) {
      if (numFormatInfo->isSigned) {
        normalizationFactors.push_back(1.0f);
        normalizationFactors.push_back(1 / 512.0f);
        normalizationFactors.push_back(1 / 512.0f);
        normalizationFactors.push_back(1 / 512.0f);
      } else {
        normalizationFactors.push_back(1 / 3.0f);
        normalizationFactors.push_back(1 / 1023.0f);
        normalizationFactors.push_back(1 / 1023.0f);
        normalizationFactors.push_back(1 / 1023.0f);
      }
    }
    return true;
  case BufDataFormat2_10_10_10:
    extractMask.push_back(10);
    extractMask.push_back(10);
    extractMask.push_back(10);
    extractMask.push_back(2);
    if (numFormatInfo->isNorm) {
      if (numFormatInfo->isSigned) {
        normalizationFactors.push_back(1 / 512.0f);
        normalizationFactors.push_back(1 / 512.0f);
        normalizationFactors.push_back(1 / 512.0f);
        normalizationFactors.push_back(1.0f);
      } else {
        normalizationFactors.push_back(1 / 1023.0f);
        normalizationFactors.push_back(1 / 1023.0f);
        normalizationFactors.push_back(1 / 1023.0f);
        normalizationFactors.push_back(1 / 3.0f);
      }
    }
    return true;
  default:
    break;
  }

  return false;
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
                                                    builderImpl.getInt32(0), lgc::Builder::BufferFlagAddress, false);
        // Create descriptor by a 64-bits pointer
        m_curAttribBufferDescr = builderImpl.buildBufferCompactDesc(descPtr, 0);
      }
      vtxDesc = m_curAttribBufferDescr;
    } else {
      // Create descriptor for vertex buffer
      vtxDesc = builderImpl.CreateBufferDesc(InternalDescriptorSetId, GenericVertexFetchShaderBinding,
                                             builderImpl.getInt32(binding), lgc::Builder::BufferFlagNonConst, false);
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
// Post-process for final fetch emulation, including for post shuffle, scaled/norm format emulation and packed format.
//
// @param description : Vertex input description.
// @param fetchInByte : Whether is doing fetch in Byte.
// @param inputCompBytes : Component Byte size of input type.
// @param numChannels : Valid number of channels.
// @param numFormatInfo : vertex format data results info corresponding to vertex numerical format (BufNumFormat).
// @param descFormatInfo : Vertex component info corresponding to vertex data format (BufDataFormat).
// @param builderImpl : BuilderImpl to use to insert vertex fetch instructions.
// @param [out] ppFetch : Destination of vertex fetch.
void VertexFetchImpl::postFetchEmulation(const VertexInputDescription *description, bool fetchInByte,
                                         unsigned inputCompBytes, unsigned numChannels,
                                         const VertexNumFormatInfo *numFormatInfo,
                                         const VertexCompFormatInfo *descFormatInfo, BuilderImpl &builderImpl,
                                         Value **ppFetch) const {
  Type *compIntTy = getVertexFetchType(false, inputCompBytes, builderImpl);
  // Do post-processing in certain cases
  std::vector<Constant *> shuffleMask;
  std::vector<unsigned> extractMask;
  std::vector<float> normalizationFactors;
  bool isPacked = needPackFormatEmulation(description, numFormatInfo, extractMask, normalizationFactors);
  // Emulation for packed formats.
  if (fetchInByte && isPacked) {
    // Must be 8 bit fetch in Byte.
    Value *packedVertex =
        PoisonValue::get(FixedVectorType::get(builderImpl.getInt8Ty(), descFormatInfo->vertexByteSize));
    for (unsigned i = 0; i < descFormatInfo->vertexByteSize; i++) {
      Value *oneByteVal = builderImpl.CreateExtractElement(*ppFetch, i);
      oneByteVal = builderImpl.CreateTrunc(oneByteVal, builderImpl.getInt8Ty());
      packedVertex = builderImpl.CreateInsertElement(packedVertex, oneByteVal, i);
    }

    // Cast fetched data from i8Vec4 to a i32 for bit extract and vector re-construction.
    packedVertex = builderImpl.CreateBitCast(packedVertex, builderImpl.getInt32Ty());
    Value *emulateVertex = PoisonValue::get(FixedVectorType::get(compIntTy, descFormatInfo->compCount));

    // Re-construct final results.
    unsigned offset = 0;
    for (unsigned i = 0; i < descFormatInfo->compCount; i++) {
      Value *bitExtractVal = builderImpl.CreateExtractBitField(
          packedVertex, builderImpl.getInt32(offset), builderImpl.getInt32(extractMask[i]), numFormatInfo->isSigned);
      emulateVertex = builderImpl.CreateInsertElement(emulateVertex, bitExtractVal, builderImpl.getInt32(i));
      offset += extractMask[i];
    }

    *ppFetch = emulateVertex;
  }
  if (needPostShuffle(description, shuffleMask)) {
    // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
    // get the components in the right order.
    assert(shuffleMask.empty() == false);
    *ppFetch = builderImpl.CreateShuffleVector(*ppFetch, *ppFetch, ConstantVector::get(shuffleMask));
  }
  bool isPatch32 = needPatch32(description);
  if (fetchInByte || isPatch32) {
    Type *compFloatTy = getVertexFetchType(true, inputCompBytes, builderImpl);
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *elemInstr = nullptr;
      if (numChannels == 1)
        elemInstr = *ppFetch;
      else
        elemInstr = builderImpl.CreateExtractElement(*ppFetch, builderImpl.getInt32(i));
      if (numFormatInfo->isNorm || numFormatInfo->isScaled) {
        // A constant divisor for normalization emulation.
        float normDiv = isPatch32 ? 2.14748365e+09f : 32767.0f;
        if (numFormatInfo->isSigned) {
          // Signed int to float
          elemInstr = builderImpl.CreateSIToFP(elemInstr, compFloatTy);
        } else {
          // Unsigned int to float
          elemInstr = builderImpl.CreateUIToFP(elemInstr, compFloatTy);
          normDiv = isPatch32 ? 4.29496730e+09f : 65535.0f;
        }
        if (numFormatInfo->isNorm) {
          // Normalization emulation.
          if (isPacked) {
            elemInstr = builderImpl.CreateFMul(elemInstr, ConstantFP::get(compFloatTy, normalizationFactors[i]));
          } else {
            elemInstr = builderImpl.CreateFDiv(elemInstr, ConstantFP::get(compFloatTy, normDiv));
          }
        }
      } else if (description->nfmt == BufNumFormatFixed) {
        // A constant divisor to translate loaded float bits to fixed point format.
        double fixedPointMul = 1.0f / 65536.0f;
        elemInstr = builderImpl.CreateSIToFP(elemInstr, compFloatTy);
        elemInstr = builderImpl.CreateFMul(elemInstr, ConstantFP::get(compFloatTy, fixedPointMul));
      }

      elemInstr = builderImpl.CreateBitCast(elemInstr, compIntTy);
      if (numChannels > 1)
        *ppFetch = builderImpl.CreateInsertElement(*ppFetch, elemInstr, builderImpl.getInt32(i));
      else
        *ppFetch = elemInstr;
    }
  }
}

// =====================================================================================================================
// Inserts instructions to do vertex fetch operations.
//
// When do vertex fetch, the construction order is : single fetch -> component -> fetch results.
// In most of cases, we do fetch in component granularity to make sure alignment is correct. After later optimization,
// if several tbuffer_load intrinsics with same load format are observed continuous on buffer by checking their offsets,
// they will be combined and generate a vector fetch in final ISA codes.
// There are two special cases:
// 1. For packed formats, we just do a single fetch with a vector result type, which is supported from HW.
// 2. If we know address low bits of vertex buffer and found current data is not aligned, we do fetch in Byte and
// emulate results later.
//
// @param vbDesc : Vertex buffer descriptor
// @param vbIndex : Index of vertex fetch in buffer
// @param srdStride: Stride from SRD. Only for offset mode.
// @param numChannels : Valid number of channels
// @param offset : Vertex attribute offset (in bytes)
// @param dfmt : Date format of vertex buffer
// @param nfmt : Numeric format of vertex buffer
// @param inputCompBytes : Number of Bytes of input component type.
// @param fetchCompBytes : Number of Bytes of fetch results' component type.
// @param isSigned: Whether current format is signed.
// @param isPacked: Whether current format is packed.
// @param fetchInByte: Do fetch in Byte if the vertex attribute offset and stride are not aligned.
// @param builderImpl : BuilderImpl to use to insert vertex fetch instructions
// @param [out] ppFetch : Destination of vertex fetch
void VertexFetchImpl::addVertexFetchInst(Value *vbDesc, Value *vbIndex, Value *srdStride, Type *inputTy,
                                         unsigned numChannels, unsigned offset, unsigned dfmt, unsigned nfmt,
                                         unsigned inputCompBytes, unsigned fetchCompBytes, bool isSigned, bool isPacked,
                                         bool fetchInByte, BuilderImpl &builderImpl, Value **ppFetch) const {
  Intrinsic::ID instId = Intrinsic::amdgcn_struct_tbuffer_load;
  Value *instOffset = builderImpl.getInt32(0);
  if (m_useSoftwareVertexBufferDescriptors) {
    // Generated offset delta will always be aligned.
    instId = Intrinsic::amdgcn_raw_tbuffer_load;
    instOffset = builderImpl.CreateMul(vbIndex, srdStride);
  }

  // For tbuffer_load, only support two types (could be vector) of fetch : d16 or i32, depending on input type.
  unsigned tbufLoadBytes = inputCompBytes <= 2 ? 2 : 4;
  unsigned fetchLoadBytes = fetchInByte ? 1 : fetchCompBytes;

  Type *tbufLoadTy = inputCompBytes <= 2 ? builderImpl.getHalfTy() : builderImpl.getInt32Ty();
  Type *fetchLoadTy = getVertexFetchType(false, fetchLoadBytes, builderImpl);
  Type *fetchCompTy = getVertexFetchType(false, fetchCompBytes, builderImpl);
  Type *inputCompTy = getVertexFetchType(false, inputCompBytes, builderImpl);

  auto resultChannels = numChannels;
  unsigned compChannels = fetchCompBytes / fetchLoadBytes;

  // Special fetch mode for packed data format like 2_10_10_10.
  if (isPacked) {
    // If process fetch packed vertex in Byte, fetch type should be <4 x i8> and do emulation later.
    compChannels = 1;
    if (!fetchInByte) {
      // For packed format, if not fetch in Byte, directly use this data format in tbuffer_load intrinsic call.
      // HW supports to do 4 dwords fetch with format conversion, intrinsic result type should be <4 x i32>.
      // (or <4 x d16> if it is a d16 fetch).
      resultChannels = 1;
      tbufLoadTy = FixedVectorType::get(tbufLoadTy, numChannels);
    }
  }

  // As 64 bit floating type will be emulated by i32 fetch, max loaded size will be 32 Bytes.
  int compOffsets[32] = {};
  for (unsigned i = 0; i < compChannels * resultChannels; ++i) {
    compOffsets[i] = offset + i * fetchLoadBytes;
  }

  SmallVector<Value *, 6> args;
  args.push_back(vbDesc);
  if (!m_useSoftwareVertexBufferDescriptors)
    args.push_back(vbIndex);
  unsigned offsetIdx = args.size();
  args.push_back(instOffset);
  args.push_back(builderImpl.getInt32(0));
  if (fetchInByte)
    args.push_back(builderImpl.getInt32(mapVertexFormat(BUF_DATA_FORMAT_8, BufNumFormatUint)));
  else
    args.push_back(builderImpl.getInt32(mapVertexFormat(dfmt, nfmt)));
  args.push_back(builderImpl.getInt32(0));

  Value *fetchVal = resultChannels > 1 ? PoisonValue::get(FixedVectorType::get(inputCompTy, resultChannels)) : nullptr;
  for (unsigned i = 0; i < resultChannels; ++i) {
    Value *compVal = compChannels > 1 ? PoisonValue::get(FixedVectorType::get(fetchLoadTy, compChannels)) : nullptr;
    unsigned compBytes = compChannels > 1 ? fetchCompBytes : tbufLoadBytes;
    for (unsigned j = 0; j < compChannels; ++j) {
      unsigned idx = i * compChannels + j;
      Value *compOffset = builderImpl.getInt32(compOffsets[idx]);
      if (m_useSoftwareVertexBufferDescriptors)
        args[offsetIdx] = builderImpl.CreateAdd(instOffset, compOffset);
      else
        args[offsetIdx] = compOffset;

      Value *tbufLoad = builderImpl.CreateIntrinsic(tbufLoadTy, instId, args);

      // Keep intermediate component type as integer before final casting.
      if (inputCompBytes <= 2) {
        tbufLoad = builderImpl.CreateBitCast(tbufLoad, builderImpl.getInt16Ty());
      }

      if (compChannels > 1) {
        tbufLoad = builderImpl.CreateTrunc(tbufLoad, fetchLoadTy);
        compVal = builderImpl.CreateInsertElement(compVal, tbufLoad, j);
      } else {
        // Avoid do wasted Trunc here and Ext back.
        compVal = tbufLoad;
      }
    }

    // When do fetch in Byte, we finally cast fetch array to a component with expected type.
    if (compChannels > 1)
      compVal = builderImpl.CreateBitCast(compVal, fetchCompTy);

    // Let each component here have same bit width as final expected fetch results.
    if (inputCompBytes < compBytes)
      compVal = builderImpl.CreateTrunc(compVal, inputCompTy);
    else if (inputCompBytes > compBytes) {
      if (inputTy->isFloatTy() && nfmt == BufNumFormatFloat) {
        compVal = builderImpl.CreateBitCast(compVal, builderImpl.getHalfTy());
        compVal = builderImpl.CreateFPExt(compVal, builderImpl.getFloatTy());
        compVal = builderImpl.CreateBitCast(compVal, inputCompTy);
      } else if (isSigned)
        compVal = builderImpl.CreateSExt(compVal, inputCompTy);
      else
        compVal = builderImpl.CreateZExt(compVal, inputCompTy);
    }

    if (resultChannels > 1)
      fetchVal = builderImpl.CreateInsertElement(fetchVal, compVal, i);
    else
      fetchVal = compVal;
  }

  *ppFetch = fetchVal;
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
// Convert D3D12_VERTEX_BUFFER_VIEW SRD to offset mode. Stride will be used to calculate offset.
//
// @param vbDesc : Original SRD
// @param builder : Builder to use to insert vertex fetch instructions
// @returns : {New SRD,stride}
std::pair<Value *, Value *> VertexFetchImpl::convertSrdToOffsetMode(Value *vbDesc, BuilderImpl &builder) {
  assert(m_useSoftwareVertexBufferDescriptors);
  // NOTE: Vertex buffer SRD is D3D12_VERTEX_BUFFER_VIEW
  // struct VertexBufferView
  // {
  //   gpusize gpuva;
  //   uint32  sizeInBytes;
  //   uint32  strideInBytes;
  // };

  GfxIpVersion gfxIp = m_lgcContext->getTargetInfo().getGfxIpVersion();
  // Stride is from the third DWORD.
  auto srdStride = builder.CreateExtractElement(vbDesc, 3);
  SqBufRsrcWord3 sqBufRsrcWord3 = {};
  sqBufRsrcWord3.bits.dstSelX = BUF_DST_SEL_X;
  sqBufRsrcWord3.bits.dstSelY = BUF_DST_SEL_Y;
  sqBufRsrcWord3.bits.dstSelZ = BUF_DST_SEL_Z;
  sqBufRsrcWord3.bits.dstSelW = BUF_DST_SEL_W;
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
