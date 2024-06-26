/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// HLSL file for the Advanced Blend Equations Shader Runtime Library (SRL).
#ifndef __ADVANCED_BLEND_HLSL_H__
#define __ADVANCED_BLEND_HLSL_H__

// These DUMMY_*_FUNC postfix stubs must be included at the end of every driver
// stub (AmdExt*) declaration to work around a Vulkan glslang issue where the
// compiler can't deal with calls to functions that don't have bodies.
// clang-format off
#if defined(AMD_VULKAN)
#define DUMMY_VOID_FUNC   { }
#define DUMMY_INT_FUNC    { return 0; }
#define DUMMY_INT2_FUNC   { return int2(0, 0); }
#define DUMMY_FLOAT4_FUNC { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
#else
#define DUMMY_VOID_FUNC   ;
#define DUMMY_INT_FUNC    ;
#define DUMMY_INT2_FUNC   ;
#define DUMMY_FLOAT4_FUNC ;
#endif
// clang-format on

// The following extension functions are general driver intrinsics
// clang-format off
float4 AmdExtFragCoord() DUMMY_FLOAT4_FUNC
int AmdExtSampleId() DUMMY_INT_FUNC

float4 AmdAdvancedBlendTexelLoad(int64_t imageDesc, int2 iCoord, int lod) DUMMY_FLOAT4_FUNC
float4 AmdAdvancedBlendTexelLoadFmask(int64_t imageDesc, int64_t fmaskDesc, int2 iCoord, int lod) DUMMY_FLOAT4_FUNC

float4 AmdAdvancedBlendCoherentTexelLoad(float4 color, int2 iCoord, int sampleId) DUMMY_FLOAT4_FUNC
void AmdAdvancedBlendCoherentTexelStore(float4 color, int2 iCoord, int sampleId) DUMMY_VOID_FUNC
    // clang-format on

    // clang-format off
enum BlendEquationEnum {
  Multiply = 1,
  Screen,
  Overlay,
  Darken,
  Lighten,
  ColorDodge,
  ColorBun,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
  HslHue,
  HslSaturation,
  HslColor,
  HslLuminosity
};
// clang-format on

float AmdAdvancedBlendMultiply(float srcComponent, float dstComponent) {
  return srcComponent * dstComponent;
}

float AmdAdvancedBlendScreen(float srcComponent, float dstComponent) {
  return srcComponent + dstComponent - (srcComponent * dstComponent);
}

float AmdAdvancedBlendOverlay(float srcComponent, float dstComponent) {
  if (dstComponent <= 0.5f) {
    return 2.0f * srcComponent * dstComponent;
  } else {
    return 1.0f - (2.0f * (1.0f - srcComponent) * (1.0f - dstComponent));
  }
}

float AmdAdvancedBlendDarken(float srcComponent, float dstComponent) {
  if (srcComponent < dstComponent) {
    return srcComponent;
  } else {
    return dstComponent;
  }
}

float AmdAdvancedBlendLighten(float srcComponent, float dstComponent) {
  if (srcComponent < dstComponent) {
    return dstComponent;
  } else {
    return srcComponent;
  }
}

float AmdAdvancedBlendColorDodge(float srcComponent, float dstComponent) {
  if (dstComponent <= 0.0f) {
    return 0.0f;
  } else if (srcComponent >= 1.0f) {
    return 1.0f;
  } else {
    float temp = dstComponent / (1.0f - srcComponent);
    if (temp < 1.0f) {
      return temp;
    } else {
      return 1.0f;
    }
  }
}

float AmdAdvancedBlendColorBurn(float srcComponent, float dstComponent) {
  if (dstComponent >= 1.0f) {
    return 1.0f;
  } else if (srcComponent <= 0.0f) {
    return 0.0f;
  } else {
    float temp = (1.0f - dstComponent) / srcComponent;
    if (temp < 1.0f) {
      return 1.0f - temp;
    } else {
      return 0.0f;
    }
  }
}

float AmdAdvancedBlendHardLight(float srcComponent, float dstComponent) {
  if (srcComponent <= 0.5f) {
    return 2.0f * srcComponent * dstComponent;
  } else {
    return 1.0f - (2.0f * (1.0f - srcComponent) * (1.0f - dstComponent));
  }
}

float AmdAdvancedBlendSoftLight(float srcComponent, float dstComponent) {
  if (srcComponent <= 0.5f) {
    return dstComponent - ((1.0f - (2.0f * srcComponent)) * dstComponent * (1.0f - dstComponent));
  } else if (dstComponent <= 0.25f) {
    return dstComponent +
           (((2.0f * srcComponent) - 1.0f) * dstComponent * (((16.0f * dstComponent) - 12.0f) * dstComponent + 3.0f));
  } else {
    return dstComponent + (2.0f * srcComponent - 1.0f) * (sqrt(dstComponent) - dstComponent);
  }
}

float AmdAdvancedBlendDifference(float srcComponent, float dstComponent) {
  return abs(dstComponent - srcComponent);
}

float AmdAdvancedBlendExclusion(float srcComponent, float dstComponent) {
  return srcComponent + dstComponent - (2.0f * srcComponent * dstComponent);
}

float AmdAdvancedBlendMinv3(float3 c) {
  return min(min(c.r, c.g), c.b);
}

float AmdAdvancedBlendMaxv3(float3 c) {
  return max(max(c.r, c.g), c.b);
}

float AmdAdvancedBlendLumv3(float3 c) {
  return dot(c, float3(0.30f, 0.59f, 0.11f));
}

float AmdAdvancedBlendSatv3(float3 c) {
  return AmdAdvancedBlendMaxv3(c) - AmdAdvancedBlendMinv3(c);
}

float3 AmdAdvancedBlendSetLum(float3 cbase, float3 clum) {
  float lbase = AmdAdvancedBlendLumv3(cbase);
  float llum = AmdAdvancedBlendLumv3(clum);
  float ldiff = llum - lbase;
  float3 color = cbase + float3(ldiff, ldiff, ldiff);
  float minComponent = AmdAdvancedBlendMinv3(color);
  float maxComponent = AmdAdvancedBlendMaxv3(color);
  float tempValue;
  if (minComponent < 0.0f) {
    tempValue = llum / (llum - minComponent);
    color.r = (color.r - llum) * tempValue + llum;
    color.g = (color.g - llum) * tempValue + llum;
    color.b = (color.b - llum) * tempValue + llum;
  } else if (maxComponent > 1.0f) {
    tempValue = (1.0f - llum) / (maxComponent - llum);
    color.r = (color.r - llum) * tempValue + llum;
    color.g = (color.g - llum) * tempValue + llum;
    color.b = (color.b - llum) * tempValue + llum;
  }
  return color;
}

float3 AmdAdvancedBlendSetLumSat(float3 cbase, float3 csat, float3 clum) {
  float minbase = AmdAdvancedBlendMinv3(cbase);
  float sbase = AmdAdvancedBlendSatv3(cbase);
  float ssat = AmdAdvancedBlendSatv3(csat);
  float3 color;
  if (sbase > 0.0f) {
    color = (cbase - minbase) * ssat / sbase;
  } else {
    color = float3(0.0f, 0.0f, 0.0f);
  }
  return AmdAdvancedBlendSetLum(color, clum);
}

float AmdAdvancedBlendDivide(float dividend, float divisor) {
  if (dividend == divisor) {
    return 1.0f;
  } else {
    return dividend / divisor;
  }
}

export float4 AmdAdvancedBlendInternal(float4 inColor, int64_t imageDescMs, int64_t imageDesc, int64_t fmaskDesc,
                                       int mode, bool isMsaa) {
  float4 srcColor = inColor;
  if (mode == 0) {
    return srcColor;
  }
  float4 fragCoord = AmdExtFragCoord();
  int2 iCoord = int2(fragCoord.x, fragCoord.y);
  float4 dstColor;
  if (isMsaa) {
    dstColor = AmdAdvancedBlendTexelLoadFmask(imageDescMs, fmaskDesc, iCoord, 0);
  } else {
    dstColor = AmdAdvancedBlendTexelLoad(imageDesc, iCoord, 0);
  }
  // TODO: Uncomment them once ROV is support in LLPC
  // int sampleId = AmdExtSampleId();
  // dstColor = AmdAdvancedBlendCoherentTexelLoad(dstColor, iCoord, sampleId);

  if (srcColor.a == 0.0f) {
    srcColor.r = 0.0f;
    srcColor.g = 0.0f;
    srcColor.b = 0.0f;
  } else {
    srcColor.r = AmdAdvancedBlendDivide(srcColor.r, srcColor.a);
    srcColor.g = AmdAdvancedBlendDivide(srcColor.g, srcColor.a);
    srcColor.b = AmdAdvancedBlendDivide(srcColor.b, srcColor.a);
  }
  if (dstColor.a == 0.0f) {
    dstColor.r = 0.0f;
    dstColor.g = 0.0f;
    dstColor.b = 0.0f;
  } else {
    dstColor.r = AmdAdvancedBlendDivide(dstColor.r, dstColor.a);
    dstColor.g = AmdAdvancedBlendDivide(dstColor.g, dstColor.a);
    dstColor.b = AmdAdvancedBlendDivide(dstColor.b, dstColor.a);
  }
  float p0 = srcColor.a * dstColor.a;
  float p1 = srcColor.a * (1.0f - dstColor.a);
  float p2 = (1.0f - srcColor.a) * dstColor.a;

  float4 blendingOutput;
  blendingOutput.r = (srcColor.r * p1) + (dstColor.r * p2);
  blendingOutput.g = (srcColor.g * p1) + (dstColor.g * p2);
  blendingOutput.b = (srcColor.b * p1) + (dstColor.b * p2);
  blendingOutput.a = p0 + p1 + p2;

  float3 tempColor;
  float3 cs = float3(srcColor.r, srcColor.g, srcColor.b);
  float3 cd = float3(dstColor.r, dstColor.g, dstColor.b);
  switch (mode) {
  case Multiply:
    tempColor.r = AmdAdvancedBlendMultiply(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendMultiply(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendMultiply(srcColor.b, dstColor.b);
    break;
  case Screen:
    tempColor.r = AmdAdvancedBlendScreen(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendScreen(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendScreen(srcColor.b, dstColor.b);
    break;
  case Overlay:
    tempColor.r = AmdAdvancedBlendOverlay(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendOverlay(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendOverlay(srcColor.b, dstColor.b);
    break;
  case Darken:
    tempColor.r = AmdAdvancedBlendDarken(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendDarken(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendDarken(srcColor.b, dstColor.b);
    break;
  case Lighten:
    tempColor.r = AmdAdvancedBlendLighten(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendLighten(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendLighten(srcColor.b, dstColor.b);
    break;
  case ColorDodge:
    tempColor.r = AmdAdvancedBlendColorDodge(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendColorDodge(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendColorDodge(srcColor.b, dstColor.b);
    break;
  case ColorBun:
    tempColor.r = AmdAdvancedBlendColorBurn(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendColorBurn(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendColorBurn(srcColor.b, dstColor.b);
    break;
  case HardLight:
    tempColor.r = AmdAdvancedBlendHardLight(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendHardLight(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendHardLight(srcColor.b, dstColor.b);
    break;
  case SoftLight:
    tempColor.r = AmdAdvancedBlendSoftLight(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendSoftLight(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendSoftLight(srcColor.b, dstColor.b);
    break;
  case Difference:
    tempColor.r = AmdAdvancedBlendDifference(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendDifference(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendDifference(srcColor.b, dstColor.b);
    break;
  case Exclusion:
    tempColor.r = AmdAdvancedBlendExclusion(srcColor.r, dstColor.r);
    tempColor.g = AmdAdvancedBlendExclusion(srcColor.g, dstColor.g);
    tempColor.b = AmdAdvancedBlendExclusion(srcColor.b, dstColor.b);
    break;
  case HslHue:
    tempColor = AmdAdvancedBlendSetLumSat(cs, cd, cd);
    break;
  case HslSaturation:
    tempColor = AmdAdvancedBlendSetLumSat(cd, cs, cd);
    break;
  case HslColor:
    tempColor = AmdAdvancedBlendSetLum(cs, cd);
    break;
  case HslLuminosity:
    tempColor = AmdAdvancedBlendSetLum(cd, cs);
    break;
  default:
    break;
  }

  blendingOutput.r += tempColor.r * p0;
  blendingOutput.g += tempColor.g * p0;
  blendingOutput.b += tempColor.b * p0;
  // AmdAdvancedBlendCoherentTexelStore(blendingOutput, iCoord, sampleId);
  return blendingOutput;
}

#endif
