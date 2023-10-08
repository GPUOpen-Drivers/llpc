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
* @file  vfx.h
* @brief Header file of vfxParser interface declaration
***********************************************************************************************************************
*/

#pragma once

#include <assert.h>
#include <map>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define VFX_VERSION 0x10000
#define VFX_REVISION 1

// Enable VFX_SUPPORT_VK_PIPELINE in default
#if !defined(VFX_SUPPORT_VK_PIPELINE)
#define VFX_SUPPORT_VK_PIPELINE 1
#endif

#include "vkgcDefs.h"

#ifdef _WIN32
#define VFXAPI __cdecl
#else
#define VFXAPI
#endif

extern int Snprintf(char *pOutput, size_t bufSize, const char *pFormat, ...);

namespace Vfx {

#if VFX_SUPPORT_VK_PIPELINE || BIL_CLIENT_INTERFACE_MAJOR_VERSION >= 40
typedef Vkgc::ShaderStage ShaderStage;
#else
#error Not implemented!
#endif

// =====================================================================================================================
// Common definition of VfxParser

static const unsigned NativeShaderStageCount = 8;        // Number of native shader stages in Vulkan
static const unsigned MaxRenderSectionCount = 16;        // Max render document section count
static const unsigned MaxBindingCount = 16;              // Max binding count
static const unsigned MaxResultCount = 16;               // Max result count
static const unsigned MaxPushConstRangCount = 16;        // Max push const range count
static const unsigned MaxVertexBufferBindingCount = 16;  // Max vertex buffer binding count
static const unsigned MaxVertexAttributeCount = 32;      // Max vertex attribute count
static const unsigned MaxSpecConstantCount = 32;         // Max spec constant count
static const unsigned VfxSizeOfVec4 = 16;                // Ehe size of vec4
static const unsigned VfxInvalidValue = 0xFFFFFFFF;      // Invalid value
static const unsigned VfxVertexBufferSetId = 0xFFFFFFFE; // Vertex buffer set id
static const unsigned VfxIndexBufferSetId = 0xFFFFFFFD;  // Index buffer set id
static const unsigned VfxDynamicArrayId = 0xFFFFFFFC;    // Dynamic array id
static const size_t MaxKeyBufSize = 256;                 // Buffer size to parse a key-value pair key in VFX file.
static const size_t MaxLineBufSize = 65536;              // Buffer size to parse a line in VFX file.

#define VFX_ASSERT(...) assert(__VA_ARGS__);
#define VFX_NEW new
#define VFX_DELETE delete
#define VFX_DELETE_ARRAY delete[]

#define VFX_NEVER_CALLED()                                                                                             \
  { VFX_ASSERT(0); }
#define VFX_NOT_IMPLEMENTED()                                                                                          \
  { VFX_ASSERT(0); }

#define _STRING(x) #x
#define STRING(x) _STRING(x)

#define SIZE_OF_ARRAY(ary) (sizeof(ary) / sizeof(ary[0]))

namespace Math {
inline unsigned Absu(int number) {
  return static_cast<unsigned>(abs(number));
}
} // namespace Math

// =====================================================================================================================
// Represents binary form of IEEE 32-bit floating point type.
union Float32Bits {
  struct {
#ifdef qLittleEndian
    unsigned mantissa : 23;
    unsigned exp : 8;
    unsigned sign : 1;
#else
    unsigned sign : 1;
    unsigned exp : 8;
    unsigned mantissa : 23;
#endif
  }; // Bit fields

  unsigned u32All; // 32-bit binary value
};

// =====================================================================================================================
// Represents binary form of IEEE 16-bit floating point type.
union Float16Bits {
  struct {
#ifdef qLittleEndian
    uint16_t mantissa : 10;
    uint16_t exp : 5;
    uint16_t sign : 1;
#else
    uint16_t sign : 1;
    uint16_t exp : 5;
    uint16_t mantissa : 10;
#endif
  }; // Bit fields

  uint16_t u16All; // 16-bit binary value
};

// =====================================================================================================================
// Represents IEEE 32-bit floating point type.
class Float32 {
public:
  // Default constructor
  Float32() { m_bits.u32All = 0; }

  // Constructor, initializes our VfxFloat32 with numeric float value
  Float32(float value) { m_bits.u32All = *reinterpret_cast<unsigned *>(&value); }

  // Constructor, initializes our VfxFloat32 with another VfxFloat32
  Float32(const Float32 &other) : m_bits(other.m_bits) {}

  // Destructor
  ~Float32() {}

  // Gets the numeric value
  float GetValue() const { return *reinterpret_cast<const float *>(&m_bits.u32All); }

  // Flush denormalized value to zero
  void FlushDenormToZero() {
    if (m_bits.exp == 0 && m_bits.mantissa != 0)
      m_bits.mantissa = 0;
  }

  // Whether the value is NaN
  bool IsNaN() const { return m_bits.exp == 0xFF && m_bits.mantissa != 0; }

  // Whether the value is infinity
  bool IsInf() const { return m_bits.exp == 0xFF && m_bits.mantissa == 0; }

  // Gets bits
  Float32Bits GetBits() const { return m_bits; }

private:
  Float32Bits m_bits; // Value
};

// =====================================================================================================================
// Represents IEEE 16-bit floating point type.
class Float16 {
public:
  // Initializes VfxFloat16 from numeric float value
  void FromFloat32(float value) {
    const Float32 f32(value);
    const int exp = f32.GetBits().exp - 127 + 1;

    m_bits.sign = f32.GetBits().sign;

    if (value == 0.0f) {
      // Zero
      m_bits.exp = 0;
      m_bits.mantissa = 0;
    } else if (f32.IsNaN()) {
      // NaN
      m_bits.exp = 0x1F;
      m_bits.mantissa = 0x3FF;
    } else if (f32.IsInf()) {
      // Infinity
      m_bits.exp = 0x1F;
      m_bits.mantissa = 0;
    } else if (exp > 16) {
      // Value is too large, -> infinity
      m_bits.exp = 0x1F;
      m_bits.mantissa = 0;
    } else {
      if (exp < -13) {
        // Denormalized (exponent = 0, mantissa = abs(int(value * 2^24))
        m_bits.exp = 0;
        m_bits.mantissa = Math::Absu(static_cast<int>(value * (1u << 24)));
      } else {
        // Normalized (exponent = exp + 14, mantissa = abs(int(value * 2^(11 - exp))))
        m_bits.exp = exp + 14;
        if (exp <= 11)
          m_bits.mantissa = Math::Absu(static_cast<int>(value * (1u << (11 - exp))));
        else
          m_bits.mantissa = Math::Absu(static_cast<int>(value / (1u << (exp - 11))));
      }
    }
  }

  // Gets the numeric value
  float GetValue() const {
    float value = 0.0f;

    if (m_bits.exp == 0 && m_bits.mantissa == 0) {
      // Zero
      value = 0.0f;
    } else if (IsNaN()) {
      // NaN
      Float32Bits nan = {};
      nan.exp = 0xFF;
      nan.mantissa = 0x3FF;
      value = *reinterpret_cast<float *>(&nan.u32All);
    } else if (IsInf()) {
      // Infinity
      Float32Bits infinity = {};
      infinity.exp = 0xFF;
      infinity.mantissa = 0;
      value = *reinterpret_cast<float *>(&infinity.u32All);
    } else {
      if (m_bits.exp != 0) {
        // Normalized (value = (mantissa | 0x400) * 2^(exponent - 25))
        if (m_bits.exp >= 25)
          value = (m_bits.mantissa | 0x400) * static_cast<float>(1u << (m_bits.exp - 25));
        else
          value = (m_bits.mantissa | 0x400) / static_cast<float>(1u << (25 - m_bits.exp));
      } else {
        // Denormalized (value = mantissa * 2^-24)
        value = m_bits.mantissa / static_cast<float>(1u << 24);
      }
    }

    return m_bits.sign ? -value : value;
  }

  // Flush denormalized value to zero
  void FlushDenormToZero() {
    if (m_bits.exp == 0 && m_bits.mantissa != 0)
      m_bits.mantissa = 0;
  }

  // Whether the value is NaN
  bool IsNaN() const { return m_bits.exp == 0x1F && m_bits.mantissa != 0; }

  // Whether the value is infinity
  bool IsInf() const { return m_bits.exp == 0x1F && m_bits.mantissa == 0; }

  // Gets bits
  Float16Bits GetBits() const { return m_bits; }

private:
  Float16Bits m_bits; // Bits
};

// =====================================================================================================================
// Represents the combination union of vec4 values.
typedef struct IUFValue_ {
  union {
    int iVec4[4];
    unsigned uVec4[4];
    int64_t i64Vec2[2];
    float fVec4[4];
    Float16 f16Vec4[4];
    double dVec2[2];
  };
  struct {
    unsigned length : 16;
    bool isInt64 : 1;
    bool isFloat : 1;
    bool isFloat16 : 1;
    bool isDouble : 1;
    bool isHex : 1;
  } props;
} IUFValue;

// =====================================================================================================================
// Represents the shader binary data
struct ShaderSource {
  ShaderStage stage; // Shader stage
  unsigned dataSize; // Size of the shader binary data
  uint8_t *pData;    // Shader binary data
};

// =====================================================================================================================
// Enumerates the type of ResultItem's resultSource
enum ResultSource : unsigned {
  ResultSourceColor = 0,
  ResultSourceDepthStencil = 1,
  ResultSourceBuffer = 2,
  ResultSourceMaxEnum = VfxInvalidValue,
};

// =====================================================================================================================
// Enumerates the type of ResultItem's compareMethod
enum ResultCompareMethod : unsigned {
  ResultCompareMethodEqual = 0,
  ResultCompareMethodNotEqual = 1,
  ResultCompareMethodMaxEnum = VfxInvalidValue,
};

// =====================================================================================================================
// Enumerates the type of Sampler's dataPattern
enum SamplerPattern : unsigned {
  SamplerNearest,
  SamplerLinear,
  SamplerNearestMipNearest,
  SamplerLinearMipLinear,
};

// =====================================================================================================================
// Enumerates the type of ImageView's dataPattern
enum ImagePattern : unsigned {
  ImageCheckBoxUnorm,
  ImageCheckBoxFloat,
  ImageCheckBoxDepth,
  ImageLinearUnorm,
  ImageLinearFloat,
  ImageLinearDepth,
  ImageSolidUnorm,
  ImageSolidFloat,
  ImageSolidDepth,
};

// =====================================================================================================================
// Represents a result item in Result section.
struct ResultItem {
  ResultSource resultSource; // Where to get the result value (Color, DepthStencil, Buffer)
  IUFValue bufferBinding;    // Buffer binding if resultSource is buffer
  IUFValue offset;           // Offset of result value
  union {
    IUFValue iVec4Value;   // Int      expected result value
    IUFValue i64Vec2Value; // Int      expected result value
    IUFValue fVec4Value;   // Uint     expected result value
    IUFValue f16Vec4Value; // Float16  expected result value
    IUFValue dVec2Value;   // Double   expected result value
  };
  ResultCompareMethod compareMethod; // How to compare result to expected value
};

// =====================================================================================================================
// Represents Result section.
struct TestResult {
  unsigned numResult;                // Number of valid result items
  ResultItem result[MaxResultCount]; // Whole test results
};

// =====================================================================================================================
// Represents one specialization constant
struct SpecConstItem {
  union {
    IUFValue i; // Int constant
    IUFValue f; // Float constant
    IUFValue d; // Double constant
  };
};

// =====================================================================================================================
// Represents specialization constants for one shader stage.
struct SpecConst {
  unsigned numSpecConst;                         // Number of specialization constants
  SpecConstItem specConst[MaxSpecConstantCount]; // All specialization constants
};

// =====================================================================================================================
// Represents one vertex binding
//
// NOTE: deprecated!!
struct VertrexBufferBinding {
  unsigned binding;           // Where to get the result value (Color, DepthStencil, Buffer)
  unsigned strideInBytes;     // Buffer binding if resultSource is buffer
  VkVertexInputRate stepRate; // Offset of result value
};

// =====================================================================================================================
// Represents one vertex attribute
//
// NOTE: deprecated!!
struct VertexAttribute {
  unsigned binding;       // Attribute binding
  VkFormat format;        // Attribute format
  unsigned location;      // Attribute location
  unsigned offsetInBytes; // Attribute offset
};

// =====================================================================================================================
// Represents vertex input state
//
// NOTE: deprecated!!
struct VertexState {
  unsigned numVbBinding;                                       // Number of vertex input bindings
  VertrexBufferBinding vbBinding[MaxVertexBufferBindingCount]; // All vertex input bindings
  unsigned numAttribute;                                       // Number of vertex input attributes
  VertexAttribute attribute[MaxVertexAttributeCount];          // All vertex input attributes
};

// =====================================================================================================================
// Represents uniform constant information in one pipeline.
struct UniformConstantState {
  unsigned numUniformConstantMaps;        // Number of default uniform maps
  Vkgc::UniformConstantMap **uniformMaps; // Pointer to array of pointers to the default uniform maps
};

// =====================================================================================================================
// Represents one BufferView section.
struct BufferView {
  IUFValue binding;                // Binding of this view, consist of set, binding, arrayIndex
  VkDescriptorType descriptorType; // Descriptor type of this view
  unsigned size;                   // Size of this buffer view, assume same size for the buffer
  VkFormat format;                 // VkFormat of this view
  unsigned dataSize;               // Data size in bytes
  uint8_t *pData;                  // Buffer data
};

// =====================================================================================================================
// Represents one ImageView section.
struct ImageView {
  IUFValue binding;                // Binding of this view, consist of set, binding, arrayIndex.
  VkDescriptorType descriptorType; // Descriptor type of this view. enum type is VkDescriptorType
  IUFValue size;                   // Size of this image
  VkImageViewType viewType;        // Image view type, enum type is VkImageViewType
  ImagePattern dataPattern;        // Image data pattern
  unsigned samples;                // Number of image samples, only 1 is supported now
  unsigned mipmap;                 // Whether this image has mipmap
};

// =====================================================================================================================
// Represents one Sampler section.
struct Sampler {
  IUFValue binding;                // Binding of this view, consist of set, binding, arrayIndex
  VkDescriptorType descriptorType; // Descriptor type of this view
  SamplerPattern dataPattern;      // Sampler pattern
};

// =====================================================================================================================
// Represents one push constant range
struct PushConstRange {
  unsigned start;    // Push constant range start
  unsigned length;   // Push constant range length
  unsigned dataSize; // Data size in byte
  unsigned *pData;   // Push constant data
};

// =====================================================================================================================
// Represents DrawState section
struct DrawState {
  unsigned instance;                                    // Instance count for draw array
  unsigned vertex;                                      // Vertex count for draw array
  unsigned firstInstance;                               // First instance in draw array
  unsigned firstVertex;                                 // First vertex in draw array
  unsigned index;                                       // Index count for draw index
  unsigned firstIndex;                                  // First index in draw index
  unsigned vertexOffset;                                // Vertex offset in draw index
  VkPrimitiveTopology topology;                         // Primitive topology
  unsigned patchControlPoints;                          // Patch control points
  IUFValue dispatch;                                    // Dispatch dimension
  unsigned width;                                       // Window width
  unsigned height;                                      // Window height
  float lineWidth;                                      // Line width
  IUFValue viewport;                                    // Viewport dimension
  SpecConst vs;                                         // Vertex shader's spec constant
  SpecConst tcs;                                        // Tessellation control shader's spec constant
  SpecConst tes;                                        // Tessellation evaluation shader's spec constant
  SpecConst gs;                                         // Geometry shader's spec constant
  SpecConst fs;                                         // Fragment shader's spec constant
  SpecConst cs;                                         // Compute shader shader's spec constant
  unsigned numPushConstRange;                           // Number of push constant range
  PushConstRange pushConstRange[MaxPushConstRangCount]; // Pipeline push constant ranges
};

// =====================================================================================================================
// Represents the state of ColorBuffer.
struct ColorBuffer {
  unsigned channelWriteMask;     // Write mask to specify destination channels
  VkFormat format;               // The format of color buffer
  const char *palFormat;         // The PAL format of color buffer
  unsigned blendEnable;          // Whether the blend is enabled on this color buffer
  unsigned blendSrcAlphaToColor; // Whether source alpha is blended to color channels for this target at draw time
};

#if VFX_SUPPORT_VK_PIPELINE
// =====================================================================================================================
// Represents GraphicsPipelineState section.
struct GraphicsPipelineState {
  VkPrimitiveTopology topology;                 // Primitive type
  VkProvokingVertexModeEXT provokingVertexMode; // Provoking vertex mode
  unsigned patchControlPoints;                  // Patch control points
  unsigned deviceIndex;                         // Device index for device group
  unsigned disableVertexReuse;                  // Disable reusing vertex shader output for indexed draws
  unsigned depthClipEnable;                     // Enable clipping based on Z coordinate
  unsigned rasterizerDiscardEnable;             // Kill all rasterized pixels
  unsigned perSampleShading;                    // Enable per sample shading
  unsigned numSamples;                          // Number of coverage samples used when rendering with this pipeline
  unsigned pixelShaderSamples;                  // Controls the pixel shader execution rate
  unsigned samplePatternIdx;                    // Index into the currently bound MSAA sample pattern table
  unsigned dynamicSampleInfo;                   // Whether to enable dynamic sample
  unsigned rasterStream;                        // Which vertex stream to rasterize
  unsigned usrClipPlaneMask;                    // Mask to indicate the enabled user defined clip planes
  unsigned alphaToCoverageEnable;               // Enable alpha to coverage
  unsigned dualSourceBlendEnable;               // Blend state bound at draw time will use a dual source blend mode
  unsigned dualSourceBlendDynamic;              // Dual source blend mode is dynamically set
  unsigned switchWinding;                       // reverse the TCS declared output primitive vertex order
  unsigned enableMultiView;                     // Whether to enable multi-view support
  Vkgc::PipelineOptions options;                // Pipeline options

  Vkgc::NggState nggState; // NGG state

  ColorBuffer colorBuffer[Vkgc::MaxColorTargets]; // Color target state.
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  Vkgc::BinaryData shaderLibrary; // Shader library SPIR-V binary
#endif
  Vkgc::RtState rtState;        // Ray tracing state
  bool dynamicVertexStride;     // Dynamic Vertex input Stride is enabled.
  bool enableUberFetchShader;   // Use uber fetch shader
  bool enableEarlyCompile;      // Enable early compile
  bool enableColorExportShader; // Enable color export shader

  float tessLevelInner[2];
  float tessLevelOuter[4];
};

// =====================================================================================================================
// Represents ComputePipelineState section.
struct ComputePipelineState {
  unsigned deviceIndex;          // Device index for device group
  Vkgc::PipelineOptions options; // Pipeline options
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  Vkgc::BinaryData shaderLibrary; // Shader library SPIR-V binary
#endif
  Vkgc::RtState rtState; // Ray tracing state
};

// =====================================================================================================================
// Represents RayTracingPipelineState section.
struct RayTracingPipelineState {
  unsigned deviceIndex;                                // Device index for device group
  Vkgc::PipelineOptions options;                       // Pipeline options
  unsigned shaderGroupCount;                           // Count of shader groups
  VkRayTracingShaderGroupCreateInfoKHR *pShaderGroups; // An array of shader groups
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  Vkgc::BinaryData shaderTraceRay; // Trace-ray SPIR-V binary
#endif
  unsigned maxRecursionDepth;     // Ray tracing max recursion depth
  unsigned indirectStageMask;     // Trace-ray indirect stage mask
  Vkgc::LlpcRaytracingMode mode;  // Raytracing Compiling mode
  Vkgc::RtState rtState;          // Ray tracing state
  unsigned payloadSizeMaxInLib;   // Pipeline library maxPayloadSize
  unsigned attributeSizeMaxInLib; // Pipeline library maxAttributeSize
  bool hasPipelineLibrary;        // Whether has pipeline library
  unsigned pipelineLibStageMask;  // Pipeline library stage mask

  /// Combination of GpuRt::ShaderLibraryFeatureFlag
  unsigned gpurtFeatureFlags;
};

#endif

}; // namespace Vfx

#if VFX_SUPPORT_VK_PIPELINE
// =====================================================================================================================
// Represents the kind of vkgc pipeline
enum VfxPipelineType : unsigned {
  VfxPipelineTypeGraphics = 0,
  VfxPipelineTypeCompute,
  VfxPipelineTypeRayTracing,
};

// =====================================================================================================================
// Represents the content of PipelineDocument.
struct VfxPipelineState {
  unsigned version;                                  // Pipeline state version
  VfxPipelineType pipelineType;                      // Pipeline type
  Vkgc::GraphicsPipelineBuildInfo gfxPipelineInfo;   // Vkgc graphics pipeline build info
  Vkgc::ComputePipelineBuildInfo compPipelineInfo;   // Vkgc compute pipeline build info
  Vkgc::RayTracingPipelineBuildInfo rayPipelineInfo; // Vkgc ray tracing pipeline build info
  unsigned numStages;                                // Number of shader source sections
  Vfx::ShaderSource *stages;                         // Shader source sections
};

typedef struct VfxPipelineState *VfxPipelineStatePtr;
#else
typedef void *VfxPipelineStatePtr;
#endif

// =====================================================================================================================
// Types used in VFX library public entry points.
enum VfxDocType { VfxDocTypeRender, VfxDocTypePipeline, VfxDocTypeGlPipeline };

// =====================================================================================================================
// Public entry points of VFX library. Use these functions (in namespace Vfx) when linking to VFX as a static library.
namespace Vfx {

bool VFXAPI vfxParseFile(const char *pFilename, unsigned int numMacro, const char *pMacros[], VfxDocType type,
                         void **ppDoc, const char **ppErrorMsg);

void VFXAPI vfxCloseDoc(void *pDoc);

#if VFX_SUPPORT_VK_PIPELINE
void VFXAPI vfxGetPipelineDoc(void *pDoc, VfxPipelineStatePtr *pPipelineState);
#endif

void VFXAPI vfxPrintDoc(void *pDoc);

} // namespace Vfx
