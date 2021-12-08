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
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <array>
#include <initializer_list>

using namespace llvm;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

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

TEST(InputUtilsTest, ParseFileInputSpecDefaultEntryPoint) {
  const StringRef inputSpec = "my.pipe.file.spv";
  auto parsedOrErr = parseInputFileSpec(inputSpec);
  ASSERT_THAT_EXPECTED(parsedOrErr, Succeeded());
  InputSpec &parsed = *parsedOrErr;

  EXPECT_EQ(parsed.rawInputSpec, inputSpec);
  EXPECT_TRUE(parsed.entryPoint.empty());
  EXPECT_EQ(parsed.filename, inputSpec);
}

TEST(InputUtilsTest, ParseFileInputSpecWithEntryPoint) {
  auto parsedOrErr = parseInputFileSpec("/my/file.spvasm,entry_point");
  ASSERT_THAT_EXPECTED(parsedOrErr, Succeeded());
  InputSpec &parsed = *parsedOrErr;

  EXPECT_EQ(parsed.entryPoint, "entry_point");
  EXPECT_EQ(parsed.filename, "/my/file.spvasm");
}

TEST(InputUtilsTest, ParseFileInputSpecWithSpaces) {
  auto parsedOrErr = parseInputFileSpec("my file.spv, my entry point");
  ASSERT_THAT_EXPECTED(parsedOrErr, Succeeded());
  InputSpec &parsed = *parsedOrErr;

  EXPECT_EQ(parsed.entryPoint, " my entry point");
  EXPECT_EQ(parsed.filename, "my file.spv");
}

TEST(InputUtilsTest, ParseFileInputSpecExtensionOnly) {
  // Edge case: filename with the extension only. This is a valid input.
  auto parsedOrErr = parseInputFileSpec(".pipe");
  ASSERT_THAT_EXPECTED(parsedOrErr, Succeeded());
  InputSpec &parsed = *parsedOrErr;

  EXPECT_EQ(parsed.rawInputSpec, ".pipe");
  EXPECT_TRUE(parsed.entryPoint.empty());
  EXPECT_EQ(parsed.filename, ".pipe");
}

TEST(InputUtilsTest, ParseFileInputSpecEmptySpec) {
  auto parsedOrErr = parseInputFileSpec("");
  EXPECT_THAT_EXPECTED(parsedOrErr, FailedWithMessage(HasSubstr("File name missing")));
}

TEST(InputUtilsTest, ParseFileInputSpecEmptyFilename) {
  auto parsedOrErr = parseInputFileSpec(",main");
  EXPECT_THAT_EXPECTED(parsedOrErr, FailedWithMessage(AllOf(HasSubstr("File name missing"), HasSubstr(",main"))));
}

TEST(InputUtilsTest, ParseFileInputSpecMissingEntryPointName) {
  auto parsedOrErr = parseInputFileSpec("file.spv,");
  EXPECT_THAT_EXPECTED(parsedOrErr,
                       FailedWithMessage(AllOf(HasSubstr("Expected entry point name"), HasSubstr("file.spv,"))));
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

TEST(InputUtilsTest, IsGlslShaderFile) {
  // Based on https://www.khronos.org/opengles/sdk/tools/Reference-Compiler/.
  //
  //Copyright (C) 2002-2005 3Dlabs Inc. Ltd.
  //Copyright (C) 2012-2013 LunarG, Inc.
  //
  //All rights reserved.
  //
  //Redistribution and use in source and binary forms, with or without
  //modification, are permitted provided that the following conditions
  //are met:
  //
  // Redistributions of source code must retain the above copyright
  // notice, this list of conditions and the following disclaimer.
  //
  // Redistributions in binary form must reproduce the above
  // copyright notice, this list of conditions and the following
  // disclaimer in the documentation and/or other materials provided
  // with the distribution.
  //
  // Neither the name of 3Dlabs Inc. Ltd. nor the names of its
  // contributors may be used to endorse or promote products derived
  // from this software without specific prior written permission.
  //
  //THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  //"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  //LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  //FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  //COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  //INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  //BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  //LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  //CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  //LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  //ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  //POSSIBILITY OF SUCH DAMAGE.
  //
  for (StringRef extension : {".vert", ".tesc", ".tese", ".geom", ".frag", ".comp"}) {
    const std::string basename = "file" + extension.str();

    // Good inputs.
    EXPECT_TRUE(isGlslShaderTextFile(basename));
    EXPECT_TRUE(isGlslShaderTextFile("/some/long/path/./test_" + basename));

    // Bad inputs.
    EXPECT_FALSE(isGlslShaderTextFile(basename + ".x"));
    EXPECT_FALSE(isGlslShaderTextFile(StringRef(basename).drop_back(1)));
  }

  // Bad inputs.
  EXPECT_FALSE(isGlslShaderTextFile("file.glsl"));
  EXPECT_FALSE(isGlslShaderTextFile("file.vs"));
  EXPECT_FALSE(isGlslShaderTextFile("file.vshader"));
  EXPECT_FALSE(isGlslShaderTextFile("file.fs"));
  EXPECT_FALSE(isGlslShaderTextFile("file.fragment"));
  EXPECT_FALSE(isGlslShaderTextFile("file.ps"));
  EXPECT_FALSE(isGlslShaderTextFile("file.pixel"));
  EXPECT_FALSE(isGlslShaderTextFile("file.spv"));
  EXPECT_FALSE(isGlslShaderTextFile("file.spvasm"));
  EXPECT_FALSE(isGlslShaderTextFile("file"));
  EXPECT_FALSE(isGlslShaderTextFile(""));
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
#endif

// Test class for groupInputSpecs tests. Manages temporary files created by tests.
class GroupInputSpecsTest : public ::testing::Test {
public:
  // Creates a new temporary file in the form `some/temp/dir/<prefix>some_chars.<extension>`. The file will be
  // automatically removed at the end of the test. Sets `finalPath` to the full path of the created file.
  void createTestFile(StringRef prefix, StringRef extension, std::string &finalPath) {
    SmallVector<char> bytes;
    std::error_code err = sys::fs::createTemporaryFile(prefix, extension, bytes);
    ASSERT_FALSE(err) << "Failed to create temporary test file: " << err;
    m_createdFiles.emplace_back(bytes.begin(), bytes.end());
    finalPath = m_createdFiles.back();
  }

  // Converts a valid filename to input spec.
  static InputSpec toInputSpec(StringRef filename) { return cantFail(parseInputFileSpec(filename)); };

  // Converts a list of valid filenames to input specs.
  static SmallVector<InputSpec> toInputSpecs(ArrayRef<std::string> filenames) {
    return cantFail(parseAndCollectInputFileSpecs(filenames));
  }

  // Cleans up all the temporary files created.
  void TearDown() override {
    for (auto &filePath : m_createdFiles) {
      std::error_code err = sys::fs::remove(filePath, false);
      ASSERT_FALSE(err) << "Failed to remove temporary test file: " << err;
    }
  }

private:
  SmallVector<std::string> m_createdFiles;
};

TEST_F(GroupInputSpecsTest, NoInputs) {
  auto groupsOrErr = groupInputSpecs({});
  ASSERT_THAT_EXPECTED(groupsOrErr, Succeeded());
  EXPECT_TRUE(groupsOrErr->empty());
}

TEST_F(GroupInputSpecsTest, NonExistentInput) {
  auto groupsOrErr = groupInputSpecs({toInputSpec("/this/path/does/not/exit.pipe")});
  EXPECT_THAT_EXPECTED(groupsOrErr, Failed());
}

TEST_F(GroupInputSpecsTest, OnePipe) {
  std::string pipePath;
  createTestFile("a", "pipe", pipePath);
  const auto pipeSpec = toInputSpec(pipePath);

  auto groupsOrErr = groupInputSpecs({pipeSpec});
  ASSERT_THAT_EXPECTED(groupsOrErr, Succeeded());
  ASSERT_EQ(groupsOrErr->size(), 1u);

  InputSpecGroup &group = groupsOrErr->front();
  EXPECT_THAT(group, ElementsAre(pipeSpec));
}

// Check that multiple pipe files are places in separate one-element groups.
TEST_F(GroupInputSpecsTest, MultiplePipe) {
  std::string inputPathA;
  createTestFile("a", "pipe", inputPathA);
  const auto specA = toInputSpec(inputPathA);

  std::string inputPathB;
  createTestFile("b", "pipe", inputPathB);
  const auto specB = toInputSpec(inputPathB);

  auto groupsOrErr = groupInputSpecs({specA, specB});
  ASSERT_THAT_EXPECTED(groupsOrErr, Succeeded());
  ASSERT_EQ(groupsOrErr->size(), 2u);

  InputSpecGroup &group1 = groupsOrErr->front();
  EXPECT_THAT(group1, ElementsAre(specA));
  InputSpecGroup &group2 = groupsOrErr->back();
  EXPECT_THAT(group2, ElementsAre(specB));
}

TEST_F(GroupInputSpecsTest, OneShader) {
  std::string shaderPath;
  createTestFile("a", "spv", shaderPath);
  const auto shaderSpec = toInputSpec(shaderPath);

  auto groupsOrErr = groupInputSpecs({shaderSpec});
  ASSERT_THAT_EXPECTED(groupsOrErr, Succeeded());
  ASSERT_EQ(groupsOrErr->size(), 1u);

  InputSpecGroup &group = groupsOrErr->front();
  EXPECT_THAT(group, ElementsAre(shaderSpec));
}

// Check that multiple shader inputs are placed in a single group.
TEST_F(GroupInputSpecsTest, MultipleShaders) {
  std::array<std::string, 3> shaderPaths = {};
  createTestFile("a", "spv", shaderPaths[0]);
  createTestFile("b", "spvasm", shaderPaths[1]);
  createTestFile("c", "frag", shaderPaths[2]);

  const auto inputSpecs = toInputSpecs({shaderPaths[0], shaderPaths[1], shaderPaths[2]});
  auto groupsOrErr = groupInputSpecs(inputSpecs);
  ASSERT_THAT_EXPECTED(groupsOrErr, Succeeded());
  ASSERT_EQ(groupsOrErr->size(), 1u);

  InputSpecGroup &group = groupsOrErr->front();
  EXPECT_THAT(group, ElementsAreArray(inputSpecs));
}

// Check that an Error is returned when mixing .pipe and shader inputs.
TEST_F(GroupInputSpecsTest, MixShaderPipe) {
  std::string shaderPath;
  createTestFile("a", "spv", shaderPath);
  std::string pipePath;
  createTestFile("b", "pipe", pipePath);

  auto groupsOrErr1 = groupInputSpecs(toInputSpecs({shaderPath, pipePath}));
  EXPECT_THAT_EXPECTED(groupsOrErr1, Failed());

  // The other order should also result in an error.
  auto groupsOrErr2 = groupInputSpecs(toInputSpecs({pipePath, shaderPath}));
  EXPECT_THAT_EXPECTED(groupsOrErr2, Failed());
}

} // namespace
} // namespace StandaloneCompiler
} // namespace Llpc
