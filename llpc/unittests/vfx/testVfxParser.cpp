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

#include "vfx.h"
#include "vfxParser.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace llvm;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::StrEq;

namespace Vfx {
namespace {

// Test class for Vfx Parser code.
class VfxParserTest : public ::testing::Test {
public:
  using SmallStr = SmallString<16>;

  // Returns a modifiable string containing the input string `str`.
  static SmallStr createStr(StringRef str) {
    SmallStr result = str;
    // Make it always null-terminated, so that `.data()` points to somewhere valid.
    result.push_back('\0');
    return result;
  }
};

// cppcheck-suppress syntaxError
TEST_F(VfxParserTest, PlaceholderPass) {
  EXPECT_TRUE(true);
}

TEST_F(VfxParserTest, SplitEmpty) {
  SmallStr empty = createStr("");
  ASSERT_EQ(empty.size(), 1u);

  std::vector<char *> fragments = split(empty.data(), ".");
  EXPECT_THAT(fragments, ElementsAre(StrEq("")));
  EXPECT_STREQ(empty.c_str(), "");
}

TEST_F(VfxParserTest, SplitLeadingDelimiter) {
  SmallStr str = createStr(".");
  ASSERT_EQ(str.size(), 2u);

  // We expect one empty string before '.' and one empty string after.
  std::vector<char *> fragments = split(str.data(), ".");
  EXPECT_STREQ(str.c_str(), "");
  EXPECT_THAT(fragments, ElementsAre(StrEq(""), StrEq("")));
}

TEST_F(VfxParserTest, SplitNoDelimiterOccurrences) {
  SmallStr str = createStr("abc");

  std::vector<char *> fragments = split(str.data(), ".");
  EXPECT_STREQ(str.c_str(), "abc");
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc")));
}

TEST_F(VfxParserTest, SplitNoDelimiters) {
  SmallStr str = createStr("abc");

  std::vector<char *> fragments = split(str.data(), "");
  EXPECT_STREQ(str.c_str(), "abc");
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc")));
}

TEST_F(VfxParserTest, SplitTrailingDelimiters) {
  SmallStr str = createStr("abc.");

  std::vector<char *> fragments = split(str.data(), ".");
  EXPECT_THAT(str, ElementsAreArray("abc\0"));
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc"), StrEq("")));
}

TEST_F(VfxParserTest, SplitTwoFragments) {
  SmallStr str = createStr("abc.d");

  std::vector<char *> fragments = split(str.data(), ".");
  EXPECT_THAT(str, ElementsAreArray("abc\0d"));
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc"), StrEq("d")));
}

TEST_F(VfxParserTest, SplitTwoFragmentsRepeatedDelimiter) {
  SmallStr str = createStr("abc..d");

  std::vector<char *> fragments = split(str.data(), ".");
  EXPECT_THAT(str, ElementsAreArray("abc\0\0d"));
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc"), StrEq("d")));
}

TEST_F(VfxParserTest, SplitTwoFragmentsMultipleDelimiters) {
  SmallStr str = createStr("abc, d");

  std::vector<char *> fragments = split(str.data(), ", ");
  EXPECT_THAT(str, ElementsAreArray("abc\0\0d"));
  EXPECT_THAT(fragments, ElementsAre(StrEq("abc"), StrEq("d")));
}

TEST_F(VfxParserTest, SplitMultipleFragmentsMultipleDelimiters) {
  SmallStr str = createStr("a,bb c, d ");

  std::vector<char *> fragments = split(str.data(), ", ");
  EXPECT_THAT(str, ElementsAreArray("a\0bb\0c\0\0d\0"));
  EXPECT_THAT(fragments, ElementsAre(StrEq("a"), StrEq("bb"), StrEq("c"), StrEq("d"), StrEq("")));
}

} // namespace
} // namespace Vfx
