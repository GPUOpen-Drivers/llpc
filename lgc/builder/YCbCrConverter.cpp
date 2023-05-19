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
* @file  YCbCrConverter.cpp
* @brief LLPC source file: Implementation of LLPC class YCbCrConverter
***********************************************************************************************************************
*/
#include "YCbCrConverter.h"
#include "lgc/LgcContext.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Implement wrapped YCbCr sample
//
// @param wrapInfo : Wrapped YCbCr sample information
Value *YCbCrConverter::wrappedSample(YCbCrWrappedSampleInfo &wrapInfo) {
  SmallVector<Value *, 4> coordsChroma;
  YCbCrSampleInfo *sampleInfo = wrapInfo.ycbcrInfo;
  sampleInfo->imageDesc = wrapInfo.imageDesc1;

  Value *chromaWidth = nullptr;
  Value *chromaHeight = nullptr;

  if ((m_metaData.word5.lumaDepth > 1) && (m_metaData.word1.planes > 1)) {
    SqImgRsrcRegHandler proxySqRsrcRegHelper(m_builder, wrapInfo.imageDesc2, m_gfxIp);
    chromaWidth = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Width);
    chromaHeight = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Height);

    chromaWidth = m_builder->CreateUIToFP(chromaWidth, m_builder->getFloatTy());
    chromaHeight = m_builder->CreateUIToFP(chromaHeight, m_builder->getFloatTy());
  } else {
    chromaWidth = wrapInfo.chromaWidth;
    chromaHeight = wrapInfo.chromaHeight;

    if (wrapInfo.subsampledX)
      chromaWidth = m_builder->CreateFMul(wrapInfo.chromaWidth, ConstantFP::get(m_builder->getFloatTy(), 0.5f));

    if (wrapInfo.subsampledY)
      chromaHeight = m_builder->CreateFMul(wrapInfo.chromaHeight, ConstantFP::get(m_builder->getFloatTy(), 0.5f));
  }

  coordsChroma.push_back(m_builder->CreateFDiv(wrapInfo.coordI, chromaWidth));
  coordsChroma.push_back(m_builder->CreateFDiv(wrapInfo.coordJ, chromaHeight));

  Value *result = nullptr;

  if (wrapInfo.planeCount == 1) {
    sampleInfo->imageDesc = wrapInfo.subsampledX ? wrapInfo.imageDesc2 : wrapInfo.imageDesc1;

    Instruction *imageOp = cast<Instruction>(createImageSampleInternal(coordsChroma, sampleInfo));
    result = m_builder->CreateShuffleVector(imageOp, imageOp, ArrayRef<int>{0, 2});
  } else if (wrapInfo.planeCount == 2) {
    sampleInfo->imageDesc = wrapInfo.imageDesc2;
    Instruction *imageOp = cast<Instruction>(createImageSampleInternal(coordsChroma, sampleInfo));
    result = m_builder->CreateShuffleVector(imageOp, imageOp, ArrayRef<int>{0, 2});
  } else if (wrapInfo.planeCount == 3) {
    sampleInfo->imageDesc = wrapInfo.imageDesc2;
    Instruction *imageOp1 = cast<Instruction>(createImageSampleInternal(coordsChroma, sampleInfo));

    sampleInfo->imageDesc = wrapInfo.imageDesc3;
    Instruction *imageOp2 = cast<Instruction>(createImageSampleInternal(coordsChroma, sampleInfo));
    result = m_builder->CreateShuffleVector(imageOp2, imageOp1, ArrayRef<int>{0, 6});
  } else {
    llvm_unreachable("Out of ranged plane count!");
  }

  return result;
}

// =====================================================================================================================
// Create YCbCr reconstruct linear X chroma sample
//
// @param xChromeInfo : Information for downsampled chroma channels in X dimension
Value *YCbCrConverter::reconstructLinearXChromaSample(XChromaSampleInfo &xChromaInfo) {
  YCbCrSampleInfo *sampleInfo = xChromaInfo.ycbcrInfo;
  Value *isEvenI = m_builder->CreateICmpEQ(
      m_builder->CreateSMod(m_builder->CreateFPToSI(xChromaInfo.coordI, m_builder->getInt32Ty()),
                            m_builder->getInt32(2)),
      m_builder->getInt32(0));

  Value *subCoordI = m_builder->CreateUnaryIntrinsic(
      Intrinsic::floor, m_builder->CreateFDiv(xChromaInfo.coordI, ConstantFP::get(m_builder->getFloatTy(), 2.0)));
  if (xChromaInfo.chromaOffsetX != ChromaLocation::CositedEven) {
    subCoordI = m_builder->CreateSelect(
        isEvenI, m_builder->CreateFSub(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), subCoordI);
  }

  Value *alpha = nullptr;
  if (xChromaInfo.chromaOffsetX == ChromaLocation::CositedEven) {
    alpha = m_builder->CreateSelect(isEvenI, ConstantFP::get(m_builder->getFloatTy(), 0.0),
                                    ConstantFP::get(m_builder->getFloatTy(), 0.5));
  } else {
    alpha = m_builder->CreateSelect(isEvenI, ConstantFP::get(m_builder->getFloatTy(), 0.25),
                                    ConstantFP::get(m_builder->getFloatTy(), 0.75));
  }

  Value *coordT = m_builder->CreateFDiv(xChromaInfo.coordJ, xChromaInfo.chromaHeight);

  SmallVector<Value *, 4> coordsChromaA;
  sampleInfo->imageDesc = xChromaInfo.imageDesc1;
  coordsChromaA.push_back(m_builder->CreateFDiv(subCoordI, xChromaInfo.chromaWidth));
  coordsChromaA.push_back(coordT);
  Instruction *imageOpA = cast<Instruction>(createImageSampleInternal(coordsChromaA, sampleInfo));

  SmallVector<Value *, 4> coordsChromaB;
  coordsChromaB.push_back(m_builder->CreateFDiv(
      m_builder->CreateFAdd(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), xChromaInfo.chromaWidth));
  coordsChromaB.push_back(coordT);
  Instruction *imageOpB = cast<Instruction>(createImageSampleInternal(coordsChromaB, sampleInfo));

  Value *result = m_builder->createFMix(imageOpB, imageOpA, alpha);

  return m_builder->CreateShuffleVector(result, result, ArrayRef<int>{0, 2});
}

// =====================================================================================================================
// Create YCbCr Reconstruct linear XY chroma sample
//
// @param xyChromeInfo : Information for downsampled chroma channels in XY dimension
Value *YCbCrConverter::reconstructLinearXYChromaSample(XYChromaSampleInfo &xyChromaInfo) {
  YCbCrSampleInfo *sampleInfo = xyChromaInfo.ycbcrInfo;

  Value *width = xyChromaInfo.chromaWidth;
  Value *height = xyChromaInfo.chromaHeight;

  Value *isEvenI = m_builder->CreateICmpEQ(
      m_builder->CreateSMod(m_builder->CreateFPToSI(xyChromaInfo.coordI, m_builder->getInt32Ty()),
                            m_builder->getInt32(2)),
      m_builder->getInt32(0));
  Value *isEvenJ = m_builder->CreateICmpEQ(
      m_builder->CreateSMod(m_builder->CreateFPToSI(xyChromaInfo.coordJ, m_builder->getInt32Ty()),
                            m_builder->getInt32(2)),
      m_builder->getInt32(0));

  Value *subCoordI = m_builder->CreateUnaryIntrinsic(
      Intrinsic::floor, m_builder->CreateFDiv(xyChromaInfo.coordI, ConstantFP::get(m_builder->getFloatTy(), 2.0)));
  Value *subCoordJ = m_builder->CreateUnaryIntrinsic(
      Intrinsic::floor, m_builder->CreateFDiv(xyChromaInfo.coordJ, ConstantFP::get(m_builder->getFloatTy(), 2.0)));

  if (xyChromaInfo.chromaOffsetX != ChromaLocation::CositedEven) {
    subCoordI = m_builder->CreateSelect(
        isEvenI, m_builder->CreateFSub(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), subCoordI);
  }

  if (xyChromaInfo.chromaOffsetY != ChromaLocation::CositedEven) {
    subCoordJ = m_builder->CreateSelect(
        isEvenJ, m_builder->CreateFSub(subCoordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0)), subCoordJ);
  }

  Value *alpha = nullptr;
  if (xyChromaInfo.chromaOffsetX == ChromaLocation::CositedEven) {
    alpha = m_builder->CreateSelect(isEvenI, ConstantFP::get(m_builder->getFloatTy(), 0.0),
                                    ConstantFP::get(m_builder->getFloatTy(), 0.5));
  } else {
    alpha = m_builder->CreateSelect(isEvenI, ConstantFP::get(m_builder->getFloatTy(), 0.25),
                                    ConstantFP::get(m_builder->getFloatTy(), 0.75));
  }

  Value *beta = nullptr;
  if (xyChromaInfo.chromaOffsetY == ChromaLocation::CositedEven) {
    beta = m_builder->CreateSelect(isEvenJ, ConstantFP::get(m_builder->getFloatTy(), 0.0),
                                   ConstantFP::get(m_builder->getFloatTy(), 0.5));
  } else {
    beta = m_builder->CreateSelect(isEvenJ, ConstantFP::get(m_builder->getFloatTy(), 0.25),
                                   ConstantFP::get(m_builder->getFloatTy(), 0.75));
  }

  SmallVector<Value *, 4> coordsChromaTL;
  SmallVector<Value *, 4> coordsChromaTR;
  SmallVector<Value *, 4> coordsChromaBL;
  SmallVector<Value *, 4> coordsChromaBR;

  Value *result = nullptr;
  if (xyChromaInfo.planeCount == 2) {
    sampleInfo->imageDesc = xyChromaInfo.imageDesc1;

    // Sample TL
    coordsChromaTL.push_back(m_builder->CreateFDiv(subCoordI, width));
    coordsChromaTL.push_back(m_builder->CreateFDiv(subCoordJ, height));
    Instruction *coordTL = cast<Instruction>(createImageSampleInternal(coordsChromaTL, sampleInfo));

    // Sample TR
    coordsChromaTR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), width));
    coordsChromaTR.push_back(m_builder->CreateFDiv(subCoordJ, height));
    Instruction *coordTR = cast<Instruction>(createImageSampleInternal(coordsChromaTR, sampleInfo));

    // Sample BL
    coordsChromaBL.push_back(m_builder->CreateFDiv(subCoordI, width));
    coordsChromaBL.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0)), height));
    Instruction *coordBL = cast<Instruction>(createImageSampleInternal(coordsChromaBL, sampleInfo));

    // Sample BR
    coordsChromaBR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), width));
    coordsChromaBR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0)), height));
    Instruction *coordBR = cast<Instruction>(createImageSampleInternal(coordsChromaBR, sampleInfo));

    // Linear interpolate
    result = bilinearBlend(alpha, beta, coordTL, coordTR, coordBL, coordBR);
    result = m_builder->CreateShuffleVector(result, result, ArrayRef<int>{0, 2});
  } else if (xyChromaInfo.planeCount == 3) {
    // Sample TL
    coordsChromaTL.push_back(m_builder->CreateFDiv(subCoordI, width));
    coordsChromaTL.push_back(m_builder->CreateFDiv(subCoordJ, height));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc1;
    Value *coordTLb = cast<Instruction>(createImageSampleInternal(coordsChromaTL, sampleInfo));

    sampleInfo->imageDesc = xyChromaInfo.imageDesc2;
    Value *coordTLr = cast<Instruction>(createImageSampleInternal(coordsChromaTL, sampleInfo));
    Value *coordTL = m_builder->CreateShuffleVector(coordTLr, coordTLb, ArrayRef<int>{0, 6});

    // Sample TR
    coordsChromaTR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), width));
    coordsChromaTR.push_back(m_builder->CreateFDiv(subCoordJ, height));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc1;
    Value *coordTRb = cast<Instruction>(createImageSampleInternal(coordsChromaTR, sampleInfo));

    sampleInfo->imageDesc = xyChromaInfo.imageDesc2;
    Value *coordTRr = cast<Instruction>(createImageSampleInternal(coordsChromaTR, sampleInfo));
    Value *coordTR = m_builder->CreateShuffleVector(coordTRr, coordTRb, ArrayRef<int>{0, 6});

    // Sample BL
    coordsChromaBL.push_back(m_builder->CreateFDiv(subCoordI, width));
    coordsChromaBL.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0)), height));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc1;
    Value *coordBLb = cast<Instruction>(createImageSampleInternal(coordsChromaBL, sampleInfo));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc2;
    Value *coordBLr = cast<Instruction>(createImageSampleInternal(coordsChromaBL, sampleInfo));
    Value *coordBL = m_builder->CreateShuffleVector(coordBLr, coordBLb, ArrayRef<int>{0, 6});

    // Sample BR
    coordsChromaBR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordI, ConstantFP::get(m_builder->getFloatTy(), 1.0)), width));
    coordsChromaBR.push_back(
        m_builder->CreateFDiv(m_builder->CreateFAdd(subCoordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0)), height));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc1;
    Value *coordBRb = cast<Instruction>(createImageSampleInternal(coordsChromaBR, sampleInfo));
    sampleInfo->imageDesc = xyChromaInfo.imageDesc2;
    Value *coordBRr = cast<Instruction>(createImageSampleInternal(coordsChromaBR, sampleInfo));
    Value *coordBR = m_builder->CreateShuffleVector(coordBRr, coordBRb, ArrayRef<int>{0, 6});

    // Linear interpolate
    result = bilinearBlend(alpha, beta, coordTL, coordTR, coordBL, coordBR);
  }

  return result;
}

// =====================================================================================================================
// Create YCbCr image sample internal
//
// @param coords : The ST coordinates
// @param ycbcrInfo : YCbCr sample information
Value *YCbCrConverter::createImageSampleInternal(SmallVectorImpl<Value *> &coordsIn, YCbCrSampleInfo *ycbcrInfo) {

  unsigned imageDim = ycbcrInfo->dim;

  Value *coords = UndefValue::get(FixedVectorType::get(coordsIn[0]->getType(), m_builder->getImageNumCoords(imageDim)));
  coords = m_builder->CreateInsertElement(coords, coordsIn[0], uint64_t(0));
  coords = m_builder->CreateInsertElement(coords, coordsIn[1], uint64_t(1));

  if (imageDim == Builder::Dim2DArray)
    coords = m_builder->CreateInsertElement(coords, m_coordZ, uint64_t(2));

  return m_builder->CreateImageSampleGather(ycbcrInfo->resultTy, ycbcrInfo->dim, ycbcrInfo->flags, coords,
                                            ycbcrInfo->imageDesc, ycbcrInfo->samplerDesc, ycbcrInfo->address,
                                            ycbcrInfo->instNameStr, ycbcrInfo->isSample);
}

// =====================================================================================================================
// YCbCrConverter
//
// @param builder : BuilderImpl instance
// @param ycbcrMetaData : YCbCr conversion metadata
// @param ycbcrSampleInfo : YCbCr sample information
// @param gfxIp : The GfxIp Version
YCbCrConverter::YCbCrConverter(BuilderImpl *builder, const SamplerYCbCrConversionMetaData &ycbcrMetaData,
                               YCbCrSampleInfo *ycbcrSampleInfo, GfxIpVersion *gfxIp)
    : m_builder(builder), m_metaData(ycbcrMetaData), m_gfxIp(gfxIp) {
  m_imgDescsChroma.resize(3);
  setYCbCrSampleInfo(ycbcrSampleInfo);
  genSamplerDescChroma();
  genImgDescChroma();
  prepareCoord();
}

// =====================================================================================================================
// Set YCbCr sample information
//
// @param ycbcrSampleInfo : YCbCr sampler information
void YCbCrConverter::setYCbCrSampleInfo(YCbCrSampleInfo *ycbcrSampleInfo) {
  m_ycbcrSampleInfo = ycbcrSampleInfo;

  m_samplerDescLuma = m_ycbcrSampleInfo->samplerDesc;
  m_imgDescLuma = m_ycbcrSampleInfo->imageDesc;

  m_samplerDescChroma = m_samplerDescLuma;
  m_imgDescsChroma[0] = m_imgDescLuma;

  m_resultType = m_ycbcrSampleInfo->resultTy;
}

// =====================================================================================================================
// Generate sampler descriptor for chroma channel
void YCbCrConverter::genSamplerDescChroma() {
  m_samplerDescChroma =
      generateSamplerDesc(m_samplerDescLuma, static_cast<SamplerFilter>(m_metaData.word1.chromaFilter),
                          m_metaData.word0.forceExplicitReconstruct);
}

// =====================================================================================================================
// Generate image descriptor for chroma channel
void YCbCrConverter::genImgDescChroma() {
  SqImgRsrcRegHandler proxySqRsrcRegHelper(m_builder, m_imgDescLuma, m_gfxIp);

  Value *width = nullptr;
  if ((m_metaData.word5.lumaDepth > 1) && (m_metaData.word1.planes > 1)) {
    width = ConstantInt::get(m_builder->getInt32Ty(), m_metaData.word4.lumaWidth);
    m_width = ConstantFP::get(m_builder->getFloatTy(), m_metaData.word4.lumaWidth);
    m_height = ConstantFP::get(m_builder->getFloatTy(), m_metaData.word4.lumaHeight);
  } else {
    width = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Width);
    Value *height = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Height);
    m_width = m_builder->CreateUIToFP(width, m_builder->getFloatTy());
    m_height = m_builder->CreateUIToFP(height, m_builder->getFloatTy());
  }

  if (m_metaData.word1.planes == 1) {
    Value *imgDataFmt = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Format);
    Value *dstSelXYZW = proxySqRsrcRegHelper.getReg(SqRsrcRegs::DstSelXYZW);
    Value *isGbGrFmt = nullptr;
    Value *isBgRgFmt = nullptr;

    switch (m_gfxIp->major) {
    case 6:
    case 7:
    case 8:
    case 9: {
      isGbGrFmt = m_builder->CreateICmpEQ(imgDataFmt,
                                          m_builder->getInt32(BuilderImpl::ImgDataFormat::IMG_DATA_FORMAT_BG_RG__CORE));

      isBgRgFmt = m_builder->CreateICmpEQ(imgDataFmt,
                                          m_builder->getInt32(BuilderImpl::ImgDataFormat::IMG_DATA_FORMAT_GB_GR__CORE));

      proxySqRsrcRegHelper.setReg(SqRsrcRegs::Format,
                                  m_builder->getInt32(BuilderImpl::ImgDataFormat::IMG_DATA_FORMAT_8_8_8_8));
      break;
    }
    case 10: {
      isGbGrFmt = m_builder->CreateICmpEQ(
          imgDataFmt, m_builder->getInt32(BuilderImpl::ImgFmtGfx10::IMG_FMT_BG_RG_UNORM__GFX10CORE));
      isBgRgFmt = m_builder->CreateICmpEQ(
          imgDataFmt, m_builder->getInt32(BuilderImpl::ImgFmtGfx10::IMG_FMT_GB_GR_UNORM__GFX10CORE));

      proxySqRsrcRegHelper.setReg(SqRsrcRegs::Format,
                                  m_builder->getInt32(BuilderImpl::ImgFmtGfx10::IMG_FMT_8_8_8_8_UNORM__GFX10CORE));
      break;
    }
    case 11: {
      isGbGrFmt = m_builder->CreateICmpEQ(
          imgDataFmt, m_builder->getInt32(BuilderImpl::ImgFmtGfx11::IMG_FMT_BG_RG_UNORM__GFX104PLUS));
      isBgRgFmt = m_builder->CreateICmpEQ(
          imgDataFmt, m_builder->getInt32(BuilderImpl::ImgFmtGfx11::IMG_FMT_GB_GR_UNORM__GFX104PLUS));

      proxySqRsrcRegHelper.setReg(SqRsrcRegs::Format,
                                  m_builder->getInt32(BuilderImpl::ImgFmtGfx11::IMG_FMT_8_8_8_8_UNORM__GFX104PLUS));
      break;
    }
    default:
      llvm_unreachable("GFX IP not supported!");
      break;
    }

    dstSelXYZW = m_builder->CreateSelect(isGbGrFmt, m_builder->getInt32(0x977), dstSelXYZW);
    dstSelXYZW = m_builder->CreateSelect(isBgRgFmt, m_builder->getInt32(0xF2E), dstSelXYZW);

    YCbCrAddressHandler addrHelper(m_builder, &proxySqRsrcRegHelper, m_gfxIp);
    addrHelper.genHeightAndPitch(m_metaData.word0.bitDepth.channelBitsR, 32, m_metaData.word2.bitCounts.xBitCount,
                                 m_metaData.word1.planes);
    proxySqRsrcRegHelper.setReg(SqRsrcRegs::Width,
                                m_builder->CreateLShr(width, ConstantInt::get(m_builder->getInt32Ty(), 1)));
    proxySqRsrcRegHelper.setReg(SqRsrcRegs::DstSelXYZW, dstSelXYZW);
    proxySqRsrcRegHelper.setReg(SqRsrcRegs::Pitch, addrHelper.getPitchCb());
    m_imgDescsChroma[1] = proxySqRsrcRegHelper.getRegister();
  }
}

// =====================================================================================================================
// Set image descriptor for chroma channel
//
// @param planeIndex : Specific plane index for setting image descriptor
// @param imageDesc : Image descriptor
void YCbCrConverter::SetImgDescChroma(unsigned planeIndex, llvm::Value *imageDesc) {
  assert(planeIndex < 3);
  m_imgDescsChroma[planeIndex] = imageDesc;
}

// =====================================================================================================================
// Get image descriptor for chroma channel
//
// @param planeIndex : Specific plane index for loading image descriptor
llvm::Value *YCbCrConverter::GetImgDescChroma(unsigned planeIndex) {
  assert(planeIndex < 3);
  return m_imgDescsChroma[planeIndex];
}

// =====================================================================================================================
// Convert from YCbCr to RGBA color space
Value *YCbCrConverter::convertColorSpace() {
  unsigned channelBits[3] = {
      m_metaData.word0.bitDepth.channelBitsR,
      m_metaData.word0.bitDepth.channelBitsG,
      m_metaData.word0.bitDepth.channelBitsB,
  };
  return convertColor(m_resultType, static_cast<SamplerYCbCrModelConversion>(m_metaData.word0.yCbCrModel),
                      static_cast<SamplerYCbCrRange>(m_metaData.word0.yCbCrRange), channelBits, m_ycbcrData);
}

// =====================================================================================================================
// Prepare the ST coords
void YCbCrConverter::prepareCoord() {
  Value *coords = m_ycbcrSampleInfo->address[Builder::ImageAddressIdxCoordinate];

  assert(Builder::getImageNumCoords(m_ycbcrSampleInfo->dim) ==
         cast<FixedVectorType>(coords->getType())->getNumElements());

  m_coordS = m_builder->CreateExtractElement(coords, m_builder->getInt64(0));
  m_coordT = m_builder->CreateExtractElement(coords, m_builder->getInt64(1));

  if (m_ycbcrSampleInfo->dim == Builder::Dim2DArray) {
    m_coordZ = m_builder->CreateExtractElement(coords, m_builder->getInt64(2));
  }

  m_coordU = transferSTtoUVCoords(m_coordS, m_width);
  m_coordV = transferSTtoUVCoords(m_coordT, m_height);

  m_coordI = transferUVtoIJCoords(static_cast<SamplerFilter>(m_metaData.word1.lumaFilter), m_coordU);
  m_coordJ = transferUVtoIJCoords(static_cast<SamplerFilter>(m_metaData.word1.lumaFilter), m_coordV);
}

// =====================================================================================================================
// Generate sampler descriptor for YCbCr conversion
//
// @param samplerDesc : Sampler descriptor
// @param filter : The type of sampler filter
// @param forceExplicitReconstruction : Enable/Disable force explicit chroma reconstruction
Value *YCbCrConverter::generateSamplerDesc(Value *samplerDesc, SamplerFilter filter, bool forceExplicitReconstruction) {
  SqImgSampRegHandler imgRegHelper(m_builder, samplerDesc, m_gfxIp);

  /// Determines if "TexFilter" should be ignored or not.
  // enum class TexFilterMode : uint32
  // {
  //     Blend = 0x0, ///< Use the filter method specified by the TexFilter enumeration
  //     Min   = 0x1, ///< Use the minimum value returned by the sampler, no blending op occurs
  //     Max   = 0x2, ///< Use the maximum value returned by the sampler, no blending op occurs
  // };
  // Force use blend mode
  // imgRegHelper.SetFilterMode(m_builder->getInt32(0b00));
  imgRegHelper.setReg(SqSampRegs::FilterMode, m_builder->getInt32(0b00));

  /// Enumeration which defines the mode for magnification and minification sampling
  // enum XyFilter : uint32
  // {
  //     XyFilterPoint = 0,          ///< Use single point sampling
  //     XyFilterLinear,             ///< Use linear sampling
  //     XyFilterAnisotropicPoint,   ///< Use anisotropic with single point sampling
  //     XyFilterAnisotropicLinear,  ///< Use anisotropic with linear sampling
  //     XyFilterCount
  // };
  if ((filter == SamplerFilter::Nearest) || forceExplicitReconstruction) {
    imgRegHelper.setReg(SqSampRegs::xyMagFilter, m_builder->getInt32(0b00));
    imgRegHelper.setReg(SqSampRegs::xyMinFilter, m_builder->getInt32(0b00));
  } else // filter == SamplerFilter::Linear
  {
    imgRegHelper.setReg(SqSampRegs::xyMagFilter, m_builder->getInt32(0b01));
    imgRegHelper.setReg(SqSampRegs::xyMinFilter, m_builder->getInt32(0b01));
  }

  return imgRegHelper.getRegister();
}

// =====================================================================================================================
// Implement range expanding operation on checking whether the encoding uses full numerical range on luma channel
//
// @param range : Specifies whether the encoding uses the full numerical range
// @param channelBits : Channel bits
// @param sampleResult : Sample results which need range expansion, assume in sequence => Cr, Y, Cb
Value *YCbCrConverter::rangeExpand(SamplerYCbCrRange range, const unsigned *channelBits, Value *sampleResult) {
  switch (range) {
  case SamplerYCbCrRange::ItuFull: {
    //              [2^(n - 1)/((2^n) - 1)]
    // convVec1 =  [         0.0         ]
    //              [2^(n - 1)/((2^n) - 1)]
    float row0Num = static_cast<float>(0x1u << (channelBits[0] - 0x1u)) / ((0x1u << channelBits[0]) - 1u);
    float row2Num = static_cast<float>(0x1u << (channelBits[2] - 0x1u)) / ((0x1u << channelBits[2]) - 1u);

    Value *convVec1 = UndefValue::get(FixedVectorType::get(m_builder->getFloatTy(), 3));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), row0Num), uint64_t(0));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), 0.0f), uint64_t(1));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), row2Num), uint64_t(2));

    //          [Cr]   convVec1[0]
    // result = [ Y] - convVec1[1]
    //          [Cb]   convVec1[2]
    return m_builder->CreateFSub(sampleResult, convVec1);
  }
  case SamplerYCbCrRange::ItuNarrow: {
    //             [(2^n - 1)/(224 x (2^(n - 8))]
    // convVec1 = [(2^n - 1)/(219 x (2^(n - 8))]
    //             [(2^n - 1)/(224 x (2^(n - 8))]
    float row0Num = static_cast<float>((0x1u << channelBits[0]) - 1u) / (224u * (0x1u << (channelBits[0] - 8)));
    float row1Num = static_cast<float>((0x1u << channelBits[1]) - 1u) / (219u * (0x1u << (channelBits[1] - 8)));
    float row2Num = static_cast<float>((0x1u << channelBits[2]) - 1u) / (224u * (0x1u << (channelBits[2] - 8)));

    Value *convVec1 = UndefValue::get(FixedVectorType::get(m_builder->getFloatTy(), 3));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), row0Num), uint64_t(0));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), row1Num), uint64_t(1));
    convVec1 = m_builder->CreateInsertElement(convVec1, ConstantFP::get(m_builder->getFloatTy(), row2Num), uint64_t(2));

    //             [(128 x (2^(n - 8))/(224 x (2^(n - 8))]
    // convVec2 = [( 16 x (2^(n - 8))/(219 x (2^(n - 8))]
    //             [(128 x (2^(n - 8))/(224 x (2^(n - 8))]
    row0Num = static_cast<float>(128u * (0x1u << (channelBits[0] - 8))) / (224u * (0x1u << (channelBits[0] - 8)));
    row1Num = static_cast<float>(16u * (0x1u << (channelBits[1] - 8))) / (219u * (0x1u << (channelBits[1] - 8)));
    row2Num = static_cast<float>(128u * (0x1u << (channelBits[2] - 8))) / (224u * (0x1u << (channelBits[2] - 8)));

    Value *convVec2 = UndefValue::get(FixedVectorType::get(m_builder->getFloatTy(), 3));
    convVec2 = m_builder->CreateInsertElement(convVec2, ConstantFP::get(m_builder->getFloatTy(), row0Num), uint64_t(0));
    convVec2 = m_builder->CreateInsertElement(convVec2, ConstantFP::get(m_builder->getFloatTy(), row1Num), uint64_t(1));
    convVec2 = m_builder->CreateInsertElement(convVec2, ConstantFP::get(m_builder->getFloatTy(), row2Num), uint64_t(2));

    //          convVec1[0]   [Cr]   convVec2[0]
    // result = convVec1[1] * [ Y] - convVec2[1]
    //          convVec1[2]   [Cb]   convVec2[2]
    return m_builder->CreateFSub(m_builder->CreateFMul(sampleResult, convVec1), convVec2);
  }

  default:
    llvm_unreachable("Unknown range expanding type!");
    return nullptr;
  }
}

// =====================================================================================================================
// Sample YCbCr data from each plane
void YCbCrConverter::sampleYCbCrData() {
  SmallVector<Value *, 4> coordsLuma;
  SmallVector<Value *, 4> coordsChroma;

  if ((m_metaData.word5.lumaDepth > 1) && (m_metaData.word1.planes > 1)) {
    SqImgRsrcRegHandler proxySqRsrcRegHelper(m_builder, m_imgDescLuma, m_gfxIp);

    Value *widthPadding = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Width);
    Value *heightPadding = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Height);
    widthPadding = m_builder->CreateUIToFP(widthPadding, m_builder->getFloatTy());
    heightPadding = m_builder->CreateUIToFP(heightPadding, m_builder->getFloatTy());

    // coordST = coordST * scaleFactor
    Value *widthScaleFactor = m_builder->CreateFDiv(m_width, widthPadding);
    Value *heightScaleFactor = m_builder->CreateFDiv(m_height, heightPadding);

    m_coordS = m_builder->CreateFMul(m_coordS, widthScaleFactor);
    m_coordT = m_builder->CreateFMul(m_coordT, heightScaleFactor);
  }

  // coordI -> coordS
  coordsLuma.push_back(m_coordS);
  // coordJ -> coordT
  coordsLuma.push_back(m_coordT);

  // Sample Y and A channels
  Value *imageOpLuma = cast<Instruction>(createImageSampleInternal(coordsLuma, m_ycbcrSampleInfo));
  imageOpLuma = m_builder->CreateShuffleVector(imageOpLuma, imageOpLuma, ArrayRef<int>{1, 3});

  // Init sample chroma info
  m_ycbcrSampleInfo->samplerDesc = m_samplerDescChroma;

  // Init chroma width and height
  Value *chromaWidth = m_builder->CreateFMul(m_width, ConstantFP::get(m_builder->getFloatTy(), 0.5f));
  Value *chromaHeight = m_builder->CreateFMul(m_height, ConstantFP::get(m_builder->getFloatTy(), 0.5f));

  // Init sample chroma info for downsampled chroma channels in the x dimension
  XChromaSampleInfo xChromaInfo = {};
  xChromaInfo.ycbcrInfo = m_ycbcrSampleInfo;
  xChromaInfo.imageDesc1 = m_imgDescsChroma[1];
  xChromaInfo.coordI = m_coordI;
  xChromaInfo.coordJ = m_coordJ;
  xChromaInfo.chromaWidth = chromaWidth;
  xChromaInfo.chromaHeight = m_height;
  xChromaInfo.chromaOffsetX = static_cast<ChromaLocation>(m_metaData.word1.xChromaOffset);

  // Init sample chroma info for downsampled chroma channels in xy dimension
  XYChromaSampleInfo xyChromaInfo = {};
  xyChromaInfo.ycbcrInfo = m_ycbcrSampleInfo;
  xyChromaInfo.imageDesc1 = m_imgDescsChroma[1];
  xyChromaInfo.imageDesc2 = m_imgDescsChroma[2];
  xyChromaInfo.coordI = m_coordI;
  xyChromaInfo.coordJ = m_coordJ;
  xyChromaInfo.chromaWidth = chromaWidth;
  xyChromaInfo.chromaHeight = chromaHeight;
  xyChromaInfo.planeCount = m_metaData.word1.planes;
  xyChromaInfo.chromaOffsetX = static_cast<ChromaLocation>(m_metaData.word1.xChromaOffset);
  xyChromaInfo.chromaOffsetY = static_cast<ChromaLocation>(m_metaData.word1.yChromaOffset);

  // Init wrapped sample chroma info
  YCbCrWrappedSampleInfo wrappedSampleInfo = {};
  wrappedSampleInfo.ycbcrInfo = m_ycbcrSampleInfo;
  wrappedSampleInfo.chromaWidth = m_width;
  wrappedSampleInfo.chromaHeight = m_height;
  wrappedSampleInfo.coordI = m_coordU;
  wrappedSampleInfo.coordJ = m_coordV;
  wrappedSampleInfo.imageDesc1 = m_imgDescsChroma[0];
  wrappedSampleInfo.imageDesc2 = m_imgDescsChroma[1];
  wrappedSampleInfo.imageDesc3 = m_imgDescsChroma[2];
  wrappedSampleInfo.planeCount = m_metaData.word1.planes;
  wrappedSampleInfo.subsampledX = m_metaData.word1.xSubSampled;
  wrappedSampleInfo.subsampledY = m_metaData.word1.ySubSampled;

  Value *imageOpChroma = nullptr;

  if (static_cast<SamplerFilter>(m_metaData.word1.lumaFilter) == SamplerFilter::Nearest) {
    if (m_metaData.word0.forceExplicitReconstruct || !(m_metaData.word1.xSubSampled || m_metaData.word1.ySubSampled)) {
      if ((static_cast<SamplerFilter>(m_metaData.word1.chromaFilter) == SamplerFilter::Nearest) ||
          !m_metaData.word1.xSubSampled) {
        // Reconstruct using nearest if needed, otherwise, just take what's already there.
        wrappedSampleInfo.subsampledX = false;
        wrappedSampleInfo.subsampledY = false;

        imageOpChroma = wrappedSample(wrappedSampleInfo);
      } else // SamplerFilter::Linear
      {
        if (m_metaData.word1.ySubSampled) {
          imageOpChroma = reconstructLinearXYChromaSample(xyChromaInfo);
        } else {
          imageOpChroma = reconstructLinearXChromaSample(xChromaInfo);
        }
      }
    } else {
      if (m_metaData.word1.xSubSampled) {
        wrappedSampleInfo.coordI =
            calculateImplicitChromaUV(static_cast<ChromaLocation>(m_metaData.word1.xChromaOffset), m_coordU);
      }

      if (m_metaData.word1.ySubSampled) {
        wrappedSampleInfo.coordJ =
            calculateImplicitChromaUV(static_cast<ChromaLocation>(m_metaData.word1.yChromaOffset), m_coordV);
      }

      imageOpChroma = wrappedSample(wrappedSampleInfo);
    }
  } else // lumaFilter == SamplerFilter::Linear
  {
    if (m_metaData.word0.forceExplicitReconstruct || !(m_metaData.word1.xSubSampled || m_metaData.word1.ySubSampled)) {
      Value *lumaA = calculateUVoffset(m_coordU);
      Value *lumaB = calculateUVoffset(m_coordV);
      Value *subIPlusOne = m_builder->CreateFAdd(m_coordI, ConstantFP::get(m_builder->getFloatTy(), 1.0f));
      Value *subJPlusOne = m_builder->CreateFAdd(m_coordJ, ConstantFP::get(m_builder->getFloatTy(), 1.0f));

      if ((static_cast<SamplerFilter>(m_metaData.word1.chromaFilter) == SamplerFilter::Nearest) ||
          !m_metaData.word1.xSubSampled) {
        if (!m_metaData.word1.xSubSampled) {
          wrappedSampleInfo.subsampledX = false;
          wrappedSampleInfo.subsampledY = false;
          imageOpChroma = wrappedSample(wrappedSampleInfo);
        } else {
          Value *subCoordI = m_coordI;
          Value *subCoordJ = m_coordJ;
          if (m_metaData.word1.xSubSampled) {
            subCoordI = m_builder->CreateFDiv(m_coordI, ConstantFP::get(m_builder->getFloatTy(), 2.0));
            subIPlusOne = m_builder->CreateFDiv(subIPlusOne, ConstantFP::get(m_builder->getFloatTy(), 2.0));
          }

          if (m_metaData.word1.ySubSampled) {
            subCoordJ = m_builder->CreateFDiv(m_coordJ, ConstantFP::get(m_builder->getFloatTy(), 2.0));
            subJPlusOne = m_builder->CreateFDiv(subJPlusOne, ConstantFP::get(m_builder->getFloatTy(), 2.0));
          }

          wrappedSampleInfo.coordI = subCoordI;
          wrappedSampleInfo.coordJ = subCoordJ;
          Value *coordTL = wrappedSample(wrappedSampleInfo);

          wrappedSampleInfo.coordI = subIPlusOne;
          Value *coordTR = wrappedSample(wrappedSampleInfo);

          wrappedSampleInfo.coordJ = subJPlusOne;
          Value *coordBR = wrappedSample(wrappedSampleInfo);

          wrappedSampleInfo.coordI = subCoordI;
          Value *coordBL = wrappedSample(wrappedSampleInfo);

          imageOpChroma = bilinearBlend(lumaA, lumaB, coordTL, coordTR, coordBL, coordBR);
        }
      } else // filter linear
      {
        if (m_metaData.word1.ySubSampled) {
          // Linear, Reconstructed xy chroma samples with explicit linear filtering
          Value *coordTL = reconstructLinearXYChromaSample(xyChromaInfo);

          xyChromaInfo.coordI = subIPlusOne;
          Value *coordTR = reconstructLinearXYChromaSample(xyChromaInfo);

          xyChromaInfo.coordJ = subJPlusOne;
          Value *coordBR = reconstructLinearXYChromaSample(xyChromaInfo);

          xyChromaInfo.coordI = m_coordI;
          Value *coordBL = reconstructLinearXYChromaSample(xyChromaInfo);

          imageOpChroma = bilinearBlend(lumaA, lumaB, coordTL, coordTR, coordBL, coordBR);
        } else {
          // Linear, Reconstructed X chroma samples with explicit linear filtering
          Value *coordTL = reconstructLinearXChromaSample(xChromaInfo);

          xChromaInfo.coordI = subIPlusOne;
          Value *coordTR = reconstructLinearXChromaSample(xChromaInfo);

          xChromaInfo.coordJ = subJPlusOne;
          Value *coordBR = reconstructLinearXChromaSample(xChromaInfo);

          xChromaInfo.coordI = m_coordI;
          Value *coordBL = reconstructLinearXChromaSample(xChromaInfo);

          imageOpChroma = bilinearBlend(lumaA, lumaB, coordTL, coordTR, coordBL, coordBR);
        }
      }
    } else {
      if (m_metaData.word1.xSubSampled) {
        wrappedSampleInfo.coordI =
            calculateImplicitChromaUV(static_cast<ChromaLocation>(m_metaData.word1.xChromaOffset), m_coordU);
      }

      if (m_metaData.word1.ySubSampled) {
        wrappedSampleInfo.coordJ =
            calculateImplicitChromaUV(static_cast<ChromaLocation>(m_metaData.word1.yChromaOffset), m_coordV);
      }

      imageOpChroma = wrappedSample(wrappedSampleInfo);
    }
  }

  // Adjust channel sequence to R,G,B,A
  m_ycbcrData = m_builder->CreateShuffleVector(imageOpLuma, imageOpChroma, ArrayRef<int>{2, 0, 3, 1});

  // Shuffle channels if necessary
  m_ycbcrData = m_builder->CreateShuffleVector(
      m_ycbcrData, m_ycbcrData,
      ArrayRef<int>{
          static_cast<int>(static_cast<ComponentSwizzle>(m_metaData.word0.componentMapping.swizzleR).getChannel()),
          static_cast<int>(static_cast<ComponentSwizzle>(m_metaData.word0.componentMapping.swizzleG).getChannel()),
          static_cast<int>(static_cast<ComponentSwizzle>(m_metaData.word0.componentMapping.swizzleB).getChannel()),
          static_cast<int>(static_cast<ComponentSwizzle>(m_metaData.word0.componentMapping.swizzleA).getChannel())});
}

// =====================================================================================================================
// Implement the color transfer operation for conversion from  YCbCr to RGB color model
//
// @param resultTy : Result type, assumed to be <4 x f32>
// @param colorModel : The color conversion model
// @param range : Specifies whether the encoding uses the full numerical range
// @param channelBits : Channel bits
// @param imageOp : Results which need color conversion, in sequence => Cr, Y, Cb
Value *YCbCrConverter::convertColor(Type *resultTy, SamplerYCbCrModelConversion colorModel, SamplerYCbCrRange range,
                                    unsigned *channelBits, Value *imageOp) {
  Value *subImage = m_builder->CreateShuffleVector(imageOp, imageOp, ArrayRef<int>{0, 1, 2});

  Type *floatTy = m_builder->getFloatTy();

  Value *minVec = UndefValue::get(FixedVectorType::get(floatTy, 3));
  minVec = m_builder->CreateInsertElement(minVec, ConstantFP::get(floatTy, -0.5), uint64_t(0));
  minVec = m_builder->CreateInsertElement(minVec, ConstantFP::get(floatTy, 0.0), uint64_t(1));
  minVec = m_builder->CreateInsertElement(minVec, ConstantFP::get(floatTy, -0.5), uint64_t(2));

  Value *maxVec = UndefValue::get(FixedVectorType::get(floatTy, 3));
  maxVec = m_builder->CreateInsertElement(maxVec, ConstantFP::get(floatTy, 0.5), uint64_t(0));
  maxVec = m_builder->CreateInsertElement(maxVec, ConstantFP::get(floatTy, 1.0), uint64_t(1));
  maxVec = m_builder->CreateInsertElement(maxVec, ConstantFP::get(floatTy, 0.5), uint64_t(2));

  Value *result = UndefValue::get(resultTy);

  switch (colorModel) {
  case SamplerYCbCrModelConversion::RgbIdentity: {
    // result[Cr] = C'_rgba [R]
    // result[Y]  = C'_rgba [G]
    // result[Cb] = C'_rgba [B]
    // result[a]  = C'_rgba [A]
    result = imageOp;
    break;
  }
  case SamplerYCbCrModelConversion::YCbCrIdentity:
  case SamplerYCbCrModelConversion::YCbCr601:
  case SamplerYCbCrModelConversion::YCbCr709:
  case SamplerYCbCrModelConversion::YCbCr2020: {
    // inputVec = RangeExpaned(C'_rgba)
    Value *inputVec = m_builder->CreateFClamp(rangeExpand(range, channelBits, subImage), minVec, maxVec);

    Value *inputCr = m_builder->CreateExtractElement(inputVec, m_builder->getInt64(0));
    Value *inputY = m_builder->CreateExtractElement(inputVec, m_builder->getInt64(1));
    Value *inputCb = m_builder->CreateExtractElement(inputVec, m_builder->getInt64(2));

    // SamplerYCbCrModelConversion::YCbCrIdentity
    Value *outputR = inputCr;
    Value *outputG = inputY;
    Value *outputB = inputCb;
    Value *outputA = m_builder->CreateExtractElement(imageOp, m_builder->getInt64(3));

    if (colorModel == SamplerYCbCrModelConversion::YCbCr601) {

      //           [            1.402f,   1.0f,               0.0f]
      // convMat = [-0.419198 / 0.587f,   1.0f, -0.202008 / 0.587f]
      //           [              0.0f,   1.0f,             1.772f]
      float row1Col0 = static_cast<float>(-0.419198 / 0.587);
      float row1Col2 = static_cast<float>(-0.202008 / 0.587);

      outputR = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, 1.402f), inputY);
      outputG = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, row1Col0), inputY);
      outputG = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, row1Col2), outputG);
      outputB = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, 1.772f), inputY);
    } else if (colorModel == SamplerYCbCrModelConversion::YCbCr709) {

      //           [              1.5748f,   1.0f,                  0.0f]
      // convMat = [-0.33480248 / 0.7152f,   1.0f, -0.13397432 / 0.7152f]
      //           [                 0.0f,   1.0f,               1.8556f]
      float row1Col0 = static_cast<float>(-0.33480248 / 0.7152);
      float row1Col2 = static_cast<float>(-0.13397432 / 0.7152);

      outputR = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, 1.5748f), inputY);
      outputG = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, row1Col0), inputY);
      outputG = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, row1Col2), outputG);
      outputB = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, 1.8556f), inputY);
    } else if (colorModel == SamplerYCbCrModelConversion::YCbCr2020) {

      //           [              1.4746f,   1.0f,                  0.0f]
      // convMat = [-0.38737742 / 0.6780f,   1.0f, -0.11156702 / 0.6780f]
      //           [                 0.0f,   1.0f,               1.8814f]
      float row1Col0 = static_cast<float>(-0.38737742 / 0.6780);
      float row1Col2 = static_cast<float>(-0.11156702 / 0.6780);

      outputR = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, 1.4746f), inputY);
      outputG = m_builder->CreateFma(inputCr, ConstantFP::get(floatTy, row1Col0), inputY);
      outputG = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, row1Col2), outputG);
      outputB = m_builder->CreateFma(inputCb, ConstantFP::get(floatTy, 1.8814f), inputY);
    }

    result = m_builder->CreateInsertElement(result, outputR, m_builder->getInt64(0));
    result = m_builder->CreateInsertElement(result, outputG, m_builder->getInt64(1));
    result = m_builder->CreateInsertElement(result, outputB, m_builder->getInt64(2));
    result = m_builder->CreateInsertElement(result, outputA, m_builder->getInt64(3));
    break;
  }

  default:
    llvm_unreachable("Unknown color model!");
    break;
  }

  return result;
}

// =====================================================================================================================
// Implement transfer form  ST coordinates to UV coordinates operation
//
// @param coordST : ST coords
// @param scale : With/height
Value *YCbCrConverter::transferSTtoUVCoords(Value *coordST, Value *scale) {
  return m_builder->CreateFMul(coordST, scale);
}

// =====================================================================================================================
// Implement the adjustment of UV coordinates when the sample location associated with
// downsampled chroma channels in the X/XY dimension occurs
//
// @param offset : The sample location associated with downsampled chroma channels in X dimension
// @param coordUV : UV coordinates
Value *YCbCrConverter::calculateImplicitChromaUV(ChromaLocation offset, Value *coordUV) {
  if (offset == ChromaLocation::CositedEven)
    coordUV = m_builder->CreateFAdd(coordUV, ConstantFP::get(m_builder->getFloatTy(), 0.5f));

  return m_builder->CreateFMul(coordUV, ConstantFP::get(m_builder->getFloatTy(), 0.5f));
}

// =====================================================================================================================
// Transfer IJ coordinates from UV coordinates
//
// @param filter : Nearest or Linear sampler filter
// @param coordUV : UV coordinates
Value *YCbCrConverter::transferUVtoIJCoords(SamplerFilter filter, Value *coordUV) {
  assert((filter == SamplerFilter::Nearest) || (filter == SamplerFilter::Linear));

  if (filter == SamplerFilter::Linear)
    coordUV = m_builder->CreateFSub(coordUV, ConstantFP::get(m_builder->getFloatTy(), 0.5f));

  return m_builder->CreateUnaryIntrinsic(Intrinsic::floor, coordUV);
}

// =====================================================================================================================
// Calculate UV offset to the top-left pixel
//
// @param coordUV : UV coordinates
Value *YCbCrConverter::calculateUVoffset(Value *coordUV) {
  Value *coordUVBiased = m_builder->CreateFSub(coordUV, ConstantFP::get(m_builder->getFloatTy(), 0.5f));
  Value *coordIJ = m_builder->CreateUnaryIntrinsic(Intrinsic::floor, coordUVBiased);
  return m_builder->CreateFSub(coordUVBiased, coordIJ);
}

// =====================================================================================================================
// Implement bilinear blend
//
// @param alpha : Horizontal weight
// @param beta : Vertical weight
// @param coordTL: Top-left pixel
// @param coordTR : Top-right pixel
// @param coordBL : Bottom-left pixel
// @param coordBR : Bottom-right pixel
Value *YCbCrConverter::bilinearBlend(Value *alpha, Value *beta, Value *coordTL, Value *coordTR, Value *coordBL,
                                     Value *coordBR) {
  Value *coordTop = m_builder->createFMix(coordTL, coordTR, alpha);
  Value *coordBot = m_builder->createFMix(coordBL, coordBR, alpha);

  return m_builder->createFMix(coordTop, coordBot, beta);
}
