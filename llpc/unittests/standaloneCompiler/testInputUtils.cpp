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

#include "llpcInputUtils.h"
#include "llvm/ADT/SmallVector.h"
#include "gmock/gmock.h"
#include <array>

using namespace llvm;

namespace Llpc {
namespace StandaloneCompiler {
namespace {

// ELF magic format described at https://en.wikipedia.org/wiki/Executable_and_Linkable_Format.
constexpr std::array<uint8_t, 4> ElfMagic = {0x7F, 'E', 'L', 'F'};
constexpr size_t ElfHeaderLength = 64;

// LLVM Bitcode magic format described at https://llvm.org/docs/BitCodeFormat.html#llvm-ir-magic-number.
constexpr std::array<uint8_t, 4> LlvmBitcodeMagic = {'B', 'C', 0xC0, 0xDE};

// cppcheck-suppress syntaxError
TEST(InputUtilsTests, PlaceholderPass) {
  EXPECT_TRUE(true);
}

TEST(InputUtilsTest, IsElfBinaryGoodMagic) {
  SmallVector<uint8_t> header(ElfMagic.begin(), ElfMagic.end());
  header.resize(ElfHeaderLength);

  EXPECT_TRUE(isElfBinary(header.data(), header.size()));
  // Pass a valid magic, but insufficient size for an ELF header.
  EXPECT_FALSE(isElfBinary(header.data(), ElfHeaderLength / 2));
}

TEST(InputUtilsTest, IsElfBinaryBadMagic) {
  SmallVector<uint8_t> header(ElfMagic.begin(), ElfMagic.end());
  header.resize(ElfHeaderLength);

  // First byte wrong.
  header[0] = 0x7E;
  EXPECT_FALSE(isElfBinary(header.data(), header.size()));

  // Last byte wrong.
  header[0] = ElfMagic[0];
  header[3] = 'G';
  EXPECT_FALSE(isElfBinary(header.data(), header.size()));

  // Last byte wrong and insufficient size.
  EXPECT_FALSE(isElfBinary(header.data(), 3));
}

TEST(InputUtilsTest, LlvmBitcodeGoodMagic) {
  SmallVector<uint8_t> header(LlvmBitcodeMagic.begin(), LlvmBitcodeMagic.end());
  EXPECT_TRUE(isLlvmBitcode(header.data(), header.size()));

  // Pass more bytes.
  header.resize(100);
  EXPECT_TRUE(isLlvmBitcode(header.data(), header.size()));
}

TEST(InputUtilsTest, LlvmBitcodeBadMagic) {
  SmallVector<uint8_t> header(LlvmBitcodeMagic.begin(), LlvmBitcodeMagic.end());

  // Wrong first byte.
  header[0] = 'X';
  EXPECT_FALSE(isLlvmBitcode(header.data(), header.size()));

  // Wrong last byte.
  header[0] = LlvmBitcodeMagic[0];
  header[3] = 0x42;
  EXPECT_FALSE(isLlvmBitcode(header.data(), header.size()));

  // Wong last byte and insufficient size.
  EXPECT_FALSE(isLlvmBitcode(header.data(), 3));
}

// ISA text should always start with a tab character.
TEST(InputUtilsTest, IsaTextGood) {
  StringRef header = "\tXYZ";
  EXPECT_TRUE(isIsaText(header.data(), header.size()));
  EXPECT_TRUE(isIsaText(header.data(), 1));
}

TEST(InputUtilsTest, IsaTextBad) {
  for (StringRef header : {"   XYZ", "        XYZ", "\nXYZ", "X\tYZ", "XYZ"}) {
    EXPECT_FALSE(isIsaText(header.data(), header.size()));
    EXPECT_FALSE(isIsaText(header.data(), 1));
  }
}

TEST(InputUtilsTest, IsSpirvTextFile) {
  // Good inputs.
  EXPECT_TRUE(isSpirvTextFile("file.spvasm"));
  EXPECT_TRUE(isSpirvTextFile("/some/long/path/./file.test_1.spvasm"));

  // Bad inputs.
  EXPECT_FALSE(isSpirvTextFile("file.spv"));
  EXPECT_FALSE(isSpirvTextFile("file.spvas"));
  EXPECT_FALSE(isSpirvTextFile("file."));
  EXPECT_FALSE(isSpirvTextFile("file"));
  EXPECT_FALSE(isSpirvTextFile(""));
}

TEST(InputUtilsTest, IsSpirvBinaryFile) {
  // Good inputs.
  EXPECT_TRUE(isSpirvBinaryFile("file.spv"));
  EXPECT_TRUE(isSpirvBinaryFile("/some/long/path/./file.test_1.spv"));

  // Bad inputs.
  EXPECT_FALSE(isSpirvBinaryFile("file.spvasm"));
  EXPECT_FALSE(isSpirvBinaryFile("file.sp"));
  EXPECT_FALSE(isSpirvBinaryFile("file."));
  EXPECT_FALSE(isSpirvBinaryFile("file"));
  EXPECT_FALSE(isSpirvBinaryFile(""));
}

TEST(InputUtilsTest, IsLlvmIrFile) {
  // Good inputs.
  EXPECT_TRUE(isLlvmIrFile("file.ll"));
  EXPECT_TRUE(isLlvmIrFile("/some/long/path/./file.test_1.ll"));

  // Bad inputs.
  EXPECT_FALSE(isLlvmIrFile("file.llvm"));
  EXPECT_FALSE(isLlvmIrFile("file.l"));
  EXPECT_FALSE(isLlvmIrFile("file."));
  EXPECT_FALSE(isLlvmIrFile("file"));
  EXPECT_FALSE(isLlvmIrFile(""));
}

TEST(InputUtilsTest, IsPipelineInfoFile) {
  // Good inputs.
  EXPECT_TRUE(isPipelineInfoFile("file.pipe"));
  EXPECT_TRUE(isPipelineInfoFile("/some/long/path/./file.test_1.pipe"));

  // Bad inputs.
  EXPECT_FALSE(isPipelineInfoFile("file.pipeline"));
  EXPECT_FALSE(isPipelineInfoFile("file.pip"));
  EXPECT_FALSE(isPipelineInfoFile("file."));
  EXPECT_FALSE(isPipelineInfoFile("file"));
  EXPECT_FALSE(isPipelineInfoFile(""));
}

TEST(InputUtilsTest, FileExtFromBinaryElf) {
  SmallVector<uint8_t> header(ElfMagic.begin(), ElfMagic.end());
  header.resize(ElfHeaderLength);
  Vkgc::BinaryData data = {header.size(), header.data()};
  EXPECT_EQ(fileExtFromBinary(data), Ext::IsaBin);
}

TEST(InputUtilsTest, FileExtFromBinaryBitcode) {
  SmallVector<uint8_t> header(LlvmBitcodeMagic.begin(), LlvmBitcodeMagic.end());
  header.resize(100);
  Vkgc::BinaryData data = {header.size(), header.data()};
  EXPECT_EQ(fileExtFromBinary(data), Ext::LlvmBitcode);
}

TEST(InputUtilsTest, FileExtFromBinaryIsaText) {
  StringRef isa = "\t.text";
  Vkgc::BinaryData data = {isa.size(), isa.data()};
  EXPECT_EQ(fileExtFromBinary(data), Ext::IsaText);
}

TEST(InputUtilsTest, FileExtFromBinaryLlvmIr) {
  StringRef ir = R"(; ModuleID = 'lgcPipeline'
target datalayout = "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-ni:7"
target triple = "amdgcn--amdpal"
  )";
  Vkgc::BinaryData data = {ir.size(), ir.data()};
  EXPECT_EQ(fileExtFromBinary(data), Ext::LlvmIr);
}

TEST(InputUtilsTest, FileExtFromBinaryUnknownFormatIsLlvmIr) {
  StringRef ir = "This should not match any other format";
  Vkgc::BinaryData data = {ir.size(), ir.data()};
  EXPECT_EQ(fileExtFromBinary(data), Ext::LlvmIr);
}

#ifndef WIN_OS
TEST(InputUtilsTest, ExpandInputFilenames) {
  // On Linux, expandInputFilenames should only copy the inputs without performing any expansions.
  std::vector<std::string> inputs = {"a.pipe", "some/path/b.pipe", "./test1", "././test2", "./files*.pipe"};
  std::vector<std::string> expanded;
  EXPECT_EQ(expandInputFilenames(inputs, expanded), Result::Success);
  EXPECT_EQ(expanded, inputs);
}
#endif // WIN_OS

} // namespace
} // namespace StandaloneCompiler
} // namespace Llpc
