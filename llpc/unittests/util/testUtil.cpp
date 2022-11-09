/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
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

#include "llpcUtil.h"
#include "vkgcDefs.h"
#include "vkgcUtil.h"
#include "lgc/EnumIterator.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace Vkgc;
using namespace llvm;

using ::testing::ElementsAre;

namespace Llpc {
namespace {

// cppcheck-suppress syntaxError
TEST(UtilTest, PlaceholderPass) {
  EXPECT_TRUE(true);
}

// cppcheck-suppress syntaxError
TEST(UtilTest, ShaderStageToMaskSingleBit) {
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageTask), ShaderStageBit::ShaderStageTaskBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageVertex), ShaderStageBit::ShaderStageVertexBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageTessControl), ShaderStageBit::ShaderStageTessControlBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageTessEval), ShaderStageBit::ShaderStageTessEvalBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageGeometry), ShaderStageBit::ShaderStageGeometryBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageMesh), ShaderStageBit::ShaderStageMeshBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageFragment), ShaderStageBit::ShaderStageFragmentBit);
  EXPECT_EQ(shaderStageToMask(ShaderStage::ShaderStageCompute), ShaderStageBit::ShaderStageComputeBit);
}

TEST(UtilTest, IsStageInMaskEmpty) {
  const unsigned emptyMask = 0;
  for (ShaderStage stage : lgc::enumRange<ShaderStage>())
    EXPECT_FALSE(isShaderStageInMask(stage, emptyMask));
}

TEST(UtilTest, IsStageInMaskStageToMask) {
  for (ShaderStage stage : lgc::enumRange<ShaderStage>())
    EXPECT_TRUE(isShaderStageInMask(stage, shaderStageToMask(stage)));
}

TEST(UtilTest, IsStageInMaskStageBit) {
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageTask, ShaderStageBit::ShaderStageTaskBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageVertex, ShaderStageBit::ShaderStageVertexBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageTessControl, ShaderStageBit::ShaderStageTessControlBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageTessEval, ShaderStageBit::ShaderStageTessEvalBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageGeometry, ShaderStageBit::ShaderStageGeometryBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageMesh, ShaderStageBit::ShaderStageMeshBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageFragment, ShaderStageBit::ShaderStageFragmentBit));
  EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageCompute, ShaderStageBit::ShaderStageComputeBit));

  // Note: Copy shader is not a native shader stage, but we handle it regardless.
  EXPECT_TRUE(
      isShaderStageInMask(ShaderStage::ShaderStageCopyShader, shaderStageToMask(ShaderStage::ShaderStageCopyShader)));
}

TEST(UtilTest, IsStageInMaskAllGraphicsBit) {
  const unsigned gfxMask = ShaderStageBit::ShaderStageAllGraphicsBit;
  for (ShaderStage stage : gfxShaderStages())
    EXPECT_TRUE(isShaderStageInMask(stage, gfxMask));

  EXPECT_FALSE(isShaderStageInMask(ShaderStage::ShaderStageCompute, gfxMask));
}

TEST(UtilTest, IsStageInMaskMultiple) {
  {
    const unsigned mask = ShaderStageBit::ShaderStageVertexBit | ShaderStageBit::ShaderStageTessEvalBit |
                          ShaderStageBit::ShaderStageFragmentBit;
    EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageVertex, mask));
    EXPECT_FALSE(isShaderStageInMask(ShaderStage::ShaderStageTessControl, mask));
    EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageTessEval, mask));
    EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageFragment, mask));
    EXPECT_FALSE(isShaderStageInMask(ShaderStage::ShaderStageCompute, mask));
  }
  {
    const unsigned mask = ShaderStageBit::ShaderStageMeshBit;
    EXPECT_FALSE(isShaderStageInMask(ShaderStage::ShaderStageTask, mask));
    EXPECT_TRUE(isShaderStageInMask(ShaderStage::ShaderStageMesh, mask));
  }
}

TEST(UtilTest, IsNativeStage) {
  for (ShaderStage stage : nativeShaderStages())
    EXPECT_TRUE(isNativeStage(stage));

  EXPECT_FALSE(isNativeStage(ShaderStage::ShaderStageCopyShader));
}

TEST(UtilTest, IsGraphicsPipelineEmptyMask) {
  const unsigned emptyMask = 0;
  EXPECT_FALSE(isGraphicsPipeline(emptyMask));
}

TEST(UtilTest, IsGraphicsPipelineSingleBit) {
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageTaskBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageVertexBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageTessControlBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageTessEvalBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageGeometryBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageMeshBit));
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageFragmentBit));

  EXPECT_FALSE(isGraphicsPipeline(ShaderStageBit::ShaderStageComputeBit));
}

TEST(UtilTest, IsGraphicsPipelineAllGraphics) {
  EXPECT_TRUE(isGraphicsPipeline(ShaderStageBit::ShaderStageAllGraphicsBit));
}

TEST(UtilTest, IsGraphicsPipelineMultiple) {
  {
    const unsigned mask = ShaderStageBit::ShaderStageVertexBit | ShaderStageBit::ShaderStageTessEvalBit |
                          ShaderStageBit::ShaderStageFragmentBit;
    EXPECT_TRUE(isGraphicsPipeline(mask));

    EXPECT_FALSE(isGraphicsPipeline(mask | ShaderStageBit::ShaderStageComputeBit));
  }
  {
    const unsigned mask = ShaderStageBit::ShaderStageTaskBit | ShaderStageBit::ShaderStageMeshBit;
    EXPECT_TRUE(isGraphicsPipeline(mask));
  }
}

TEST(UtilTest, IsComputePipelineEmptyMask) {
  const unsigned emptyMask = 0;
  EXPECT_FALSE(isComputePipeline(emptyMask));
}

TEST(UtilTest, IsComputePipelineSingleBit) {
  EXPECT_TRUE(isComputePipeline(ShaderStageBit::ShaderStageComputeBit));

  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageTaskBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageVertexBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageTessControlBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageTessEvalBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageGeometryBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageMeshBit));
  EXPECT_FALSE(isComputePipeline(ShaderStageBit::ShaderStageFragmentBit));
}

TEST(UtilTest, IsComputePipelineTwoStages) {
  for (ShaderStage gfxStage : gfxShaderStages())
    EXPECT_FALSE(isComputePipeline(shaderStageToMask(gfxStage) | ShaderStageBit::ShaderStageComputeBit));
}

TEST(UtilTest, MaskToShaderStagesEmpty) {
  const auto stages = maskToShaderStages(0);
  EXPECT_TRUE(stages.empty());
}

TEST(UtilTest, MaskToShaderStagesOneStage) {
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageTaskBit), ElementsAre(ShaderStage::ShaderStageTask));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageVertexBit), ElementsAre(ShaderStage::ShaderStageVertex));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageTessControlBit),
              ElementsAre(ShaderStage::ShaderStageTessControl));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageTessEvalBit),
              ElementsAre(ShaderStage::ShaderStageTessEval));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageGeometryBit),
              ElementsAre(ShaderStage::ShaderStageGeometry));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageMeshBit), ElementsAre(ShaderStage::ShaderStageMesh));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageFragmentBit),
              ElementsAre(ShaderStage::ShaderStageFragment));
  EXPECT_THAT(maskToShaderStages(ShaderStageBit::ShaderStageComputeBit), ElementsAre(ShaderStage::ShaderStageCompute));

  // Note: Copy shader is not a native shader stage, but we handle it regardless.
  EXPECT_THAT(maskToShaderStages(shaderStageToMask(ShaderStage::ShaderStageCopyShader)),
              ElementsAre(ShaderStage::ShaderStageCopyShader));
}

TEST(UtilTest, MaskToShaderStagesAllGraphics) {
  const auto stages = maskToShaderStages(ShaderStageBit::ShaderStageAllGraphicsBit);
  EXPECT_THAT(stages, ElementsAre(ShaderStage::ShaderStageTask, ShaderStage::ShaderStageVertex,
                                  ShaderStage::ShaderStageTessControl, ShaderStage::ShaderStageTessEval,
                                  ShaderStage::ShaderStageGeometry, ShaderStage::ShaderStageMesh,
                                  ShaderStage::ShaderStageFragment));
}

TEST(UtilTest, MaskToShaderStagesMultiple) {
  {
    const unsigned mask = ShaderStageBit::ShaderStageVertexBit | ShaderStageBit::ShaderStageTessEvalBit |
                          ShaderStageBit::ShaderStageFragmentBit;
    const auto stages = maskToShaderStages(mask);
    EXPECT_THAT(stages, ElementsAre(ShaderStage::ShaderStageVertex, ShaderStage::ShaderStageTessEval,
                                    ShaderStage::ShaderStageFragment));
  }
  {
    const unsigned mask = ShaderStageBit::ShaderStageTaskBit | ShaderStageBit::ShaderStageMeshBit;
    const auto stages = maskToShaderStages(mask);
    EXPECT_THAT(stages, ElementsAre(ShaderStage::ShaderStageTask, ShaderStage::ShaderStageMesh));
  }
}

} // namespace
} // namespace Llpc
