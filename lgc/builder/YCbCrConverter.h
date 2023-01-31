/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  YCbCrConverter.h
 * @brief LLPC header file: contains the definition of LLPC utility class YCbCrConverter.
 ***********************************************************************************************************************
 */

#pragma once

#include "YCbCrAddressHandler.h"
#include "lgc/builder/BuilderImpl.h"

namespace lgc {

// This struct is used to parse the YCbCr conversion metadata from driver
// TODO: Remove this struct from llpc/include/vkgcDefs.h and add to client side.
struct SamplerYCbCrConversionMetaData {
  union {
    struct {                     // e.g R12X4G12X4_UNORM_2PACK16
      unsigned channelBitsR : 5; // channelBitsR = 12
      unsigned channelBitsG : 5; // channelBitsG = 12
      unsigned channelBitsB : 5; // channelBitsB =  0
      unsigned : 17;
    } bitDepth;
    struct {
      unsigned : 15;
      unsigned swizzleR : 3; // swizzleR = 3
      unsigned swizzleG : 3; // swizzleG = 4
      unsigned swizzleB : 3; // swizzleB = 5
      unsigned swizzleA : 3; // swizzleA = 6
      unsigned : 5;
    } componentMapping;
    struct {
      unsigned : 27;
      unsigned yCbCrModel : 3;               // RGB_IDENTITY(0), ycbcr_identity(1),
                                             // _709(2),_601(3),_2020(4)
      unsigned yCbCrRange : 1;               // ITU_FULL(0), ITU_NARROW(0)
      unsigned forceExplicitReconstruct : 1; // Disable(0), Enable(1)
    };
    unsigned u32All;
  } word0;

  union {
    struct {
      unsigned planes : 2;        // Number of planes, normally from 1 to 3
      unsigned lumaFilter : 1;    // FILTER_NEAREST(0) or FILTER_LINEAR(1)
      unsigned chromaFilter : 1;  // FILTER_NEAREST(0) or FILTER_LINEAR(1)
      unsigned xChromaOffset : 1; // COSITED_EVEN(0) or MIDPOINT(1)
      unsigned yChromaOffset : 1; // COSITED_EVEN(0) or MIDPOINT(1)
      unsigned xSubSampled : 1;   // true(1) or false(0)
      unsigned : 1;               // Unused
      unsigned ySubSampled : 1;   // true(1) or false(0)
      unsigned dstSelXYZW : 12;   // dst selection Swizzle
      unsigned undefined : 11;
    };
    unsigned u32All;
  } word1;

  union {
    /// For YUV formats, bitCount may not equal to bitDepth, where bitCount >= bitDepth
    struct {
      unsigned xBitCount : 6; // Bit count for x channel
      unsigned yBitCount : 6; // Bit count for y channel
      unsigned zBitCount : 6; // Bit count for z channel
      unsigned wBitCount : 6; // Bit count for w channel
      unsigned undefined : 8;
    } bitCounts;
    unsigned u32All;
  } word2;

  union {
    struct {
      unsigned sqImgRsrcWord1 : 32; // Reconstructed sqImgRsrcWord1
    };
    unsigned u32All;
  } word3;

  union {
    struct {
      unsigned lumaWidth : 16;  ///< Actual width of luma plane
      unsigned lumaHeight : 16; ///< Actual height of luma plane
    };
    unsigned u32All;
  } word4;

  union {
    struct {
      unsigned lumaDepth : 16; ///< Actual array slices of luma plane
      unsigned : 16;
    };
    unsigned u32All;
  } word5;
};

// Represents the type of sampler filter.
enum class SamplerFilter {
  Nearest = 0,
  Linear,
};

// Represents the YCbCr conversion model.
enum class SamplerYCbCrModelConversion : unsigned {
  RgbIdentity = 0,
  YCbCrIdentity,
  YCbCr709,
  YCbCr601,
  YCbCr2020,
};

// Represents whether color channels are encoded using the full range of numerical
// values or whether values are reserved for headroom and foot room.
enum class SamplerYCbCrRange {
  ItuFull = 0,
  ItuNarrow,
};

// Represents the location of downsampled chroma channel samples relative to the luma samples.
enum class ChromaLocation {
  CositedEven = 0,
  Midpoint,
};

// Represents the component values placed in each component of the output vector.
class ComponentSwizzle {
public:
  enum Channel : unsigned {
    Zero = 0,
    One = 1,
    R = 4,
    G = 5,
    B = 6,
    A = 7,
  };

  ComponentSwizzle() = default;
  ComponentSwizzle(const Channel swizzle) : m_value(swizzle), m_channel(swizzle - Channel::R){};
  ComponentSwizzle(const unsigned swizzle) {
    assert(swizzle >= Channel::Zero && swizzle <= Channel::A);
    m_value = static_cast<Channel>(swizzle);

    m_channel = (swizzle >= Channel::R) ? (m_value - Channel::R) : (m_value + Channel::R);
  }

  ComponentSwizzle &operator=(unsigned op) {
    assert(op >= Channel::Zero && op <= Channel::A);
    m_value = static_cast<Channel>(op);
    m_channel = (op >= Channel::R) ? (m_value - Channel::R) : (m_value + Channel::R);
    return *this;
  }
  constexpr bool operator!=(ComponentSwizzle::Channel op) const { return m_value != op; }
  constexpr bool operator==(ComponentSwizzle::Channel op) const { return m_value == op; }

  unsigned getChannel() const { return m_channel; }

private:
  ComponentSwizzle::Channel m_value;
  unsigned m_channel;
};

// General YCbCr sample info
struct YCbCrSampleInfo {
  llvm::Type *resultTy;
  unsigned dim;
  unsigned flags;
  llvm::Value *imageDesc;
  llvm::Value *samplerDesc;
  llvm::ArrayRef<llvm::Value *> address;
  const std::string &instNameStr;
  bool isSample;
};

// YCbCr sample info for downsampled chroma channels in the X dimension
struct XChromaSampleInfo {
  YCbCrSampleInfo *ycbcrInfo;
  llvm::Value *imageDesc1;
  llvm::Value *coordI;
  llvm::Value *coordJ;
  llvm::Value *chromaWidth;
  llvm::Value *chromaHeight;
  ChromaLocation chromaOffsetX;
};

// YCbCr sample info for downsampled chroma channels in both X and Y dimension
struct XYChromaSampleInfo : XChromaSampleInfo {
  llvm::Value *imageDesc2;
  unsigned planeCount;
  ChromaLocation chromaOffsetY;
};

// YCbCr wrapped sample info
struct YCbCrWrappedSampleInfo : XYChromaSampleInfo {
  llvm::Value *imageDesc3;
  bool subsampledX;
  bool subsampledY;
};

// =====================================================================================================================
// Represents LLPC sampler YCbCr conversion helper class
class YCbCrConverter {
public:
  YCbCrConverter(ImageBuilder *builder, const SamplerYCbCrConversionMetaData &ycbcrMetaData,
                 YCbCrSampleInfo *ycbcrSampleInfo, GfxIpVersion *gfxIp);

  // Make sure default constructor deleted - const ref can't be initialized
  YCbCrConverter() = delete;

  // Convert from YCbCr to RGBA color space
  llvm::Value *convertColorSpace();

  // Sample YCbCr data from each plane
  // Note: Should be called after genImgDescChroma and genSamplerDescChroma completes
  void sampleYCbCrData();

  // Set image descriptor for chroma channel
  void SetImgDescChroma(unsigned planeIndex, llvm::Value *imageDesc);

  // Get image descriptor for chroma channel
  llvm::Value *GetImgDescChroma(unsigned planeIndex);

private:
  // Set YCbCr sample information
  void setYCbCrSampleInfo(YCbCrSampleInfo *ycbcrSampleInfo);

  // Generate sampler descriptor for chroma channel
  void genSamplerDescChroma();

  // Generate image descriptor for chroma channel
  void genImgDescChroma();

  // Prepare the sample coords
  void prepareCoord();

  // Implement transfer from ST coordinates to UV coordinates operation
  llvm::Value *transferSTtoUVCoords(llvm::Value *coordST, llvm::Value *scale);

  // Implement the adjustment of UV coordinates when the sample location associated with
  // downsampled chroma channels in the X/XY dimension occurs
  llvm::Value *calculateImplicitChromaUV(ChromaLocation offset, llvm::Value *coordUV);

  // Transfer IJ coordinates from UV coordinates
  llvm::Value *transferUVtoIJCoords(SamplerFilter filter, llvm::Value *coordUV);

  // Calculate UV offset to top-left pixel
  llvm::Value *calculateUVoffset(llvm::Value *coordUV);

  // Implement bilinear blending
  llvm::Value *bilinearBlend(llvm::Value *alpha, llvm::Value *beta, llvm::Value *coordTL, llvm::Value *coordTR,
                             llvm::Value *coordBL, llvm::Value *coordBR);

  // Implement wrapped YCbCr sample
  llvm::Value *wrappedSample(YCbCrWrappedSampleInfo &wrapInfo);

  // Implement reconstructed YCbCr sample operation for downsampled chroma channels in the X dimension
  llvm::Value *reconstructLinearXChromaSample(XChromaSampleInfo &xChromaInfo);

  // Implement reconstructed YCbCr sample operation for downsampled chroma channels in both X and Y dimension
  llvm::Value *reconstructLinearXYChromaSample(XYChromaSampleInfo &xyChromaInfo);

  // Implement internal image sample for YCbCr conversion
  llvm::Value *createImageSampleInternal(llvm::SmallVectorImpl<llvm::Value *> &coords, YCbCrSampleInfo *ycbcrInfo);

  // Generate sampler descriptor for YCbCr conversion
  llvm::Value *generateSamplerDesc(llvm::Value *samplerDesc, SamplerFilter filter, bool forceExplicitReconstruction);

  // Implement range expanding operation on checking whether the encoding uses full numerical range on chroma channel
  llvm::Value *rangeExpand(SamplerYCbCrRange range, const unsigned *channelBits, llvm::Value *sampleResult);

  // Implement the color transfer operation for conversion from YCbCr to RGB color model
  llvm::Value *convertColor(llvm::Type *resultTy, SamplerYCbCrModelConversion colorModel, SamplerYCbCrRange range,
                            unsigned *channelBits, llvm::Value *imageOp);

  // Builder context
  ImageBuilder *m_builder;

  // Sampler YCbCr conversion meta data
  const SamplerYCbCrConversionMetaData &m_metaData;

  // Current GFX ip version
  GfxIpVersion *m_gfxIp;

  // Sampler YCbCr conversion information
  YCbCrSampleInfo *m_ycbcrSampleInfo = nullptr;

  // Plane width and height
  llvm::Value *m_width = nullptr;
  llvm::Value *m_height = nullptr;

  // Sampler descriptor for Luma channel
  llvm::Value *m_samplerDescLuma = nullptr;

  // Image descriptor for Luma channel
  llvm::Value *m_imgDescLuma = nullptr;

  // Sampler descriptor for Chroma channel
  llvm::Value *m_samplerDescChroma = nullptr;

  // Image descriptors for Chroma channel
  llvm::SmallVector<llvm::Value *, 3> m_imgDescsChroma;

  // Sample coordinates
  llvm::Value *m_coordS = nullptr;
  llvm::Value *m_coordT = nullptr;
  llvm::Value *m_coordU = nullptr;
  llvm::Value *m_coordV = nullptr;
  llvm::Value *m_coordI = nullptr;
  llvm::Value *m_coordJ = nullptr;
  llvm::Value *m_coordZ = nullptr;

  // Sample result type
  llvm::Value *m_ycbcrData = nullptr;

  // YCbCr sample result
  llvm::Type *m_resultType = nullptr;
};

} // namespace lgc
