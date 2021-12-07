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
#include "vkgcMetroHash.h"
#include "llvm/ADT/DenseSet.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <set>
#include <unordered_set>

using namespace llvm;

namespace Llpc {
namespace {

using MetroHash::Hash;
using ::testing::UnorderedElementsAre;

// cppcheck-suppress syntaxError
TEST(MetroHashTest, PlaceholderPass) {
  EXPECT_TRUE(true);
}

// Check that std::hash of a value-initialized MetroHash is 0.
TEST(MetroHashTest, CompactZero) {
  Hash hash = {};
  EXPECT_EQ(std::hash<Hash>{}(hash), 0u);

  hash.qwords[0] = 42;
  EXPECT_NE(std::hash<Hash>{}(hash), 0u);
}

// Check that hashes are comparable.
TEST(MetroHashTest, Comparisons) {
  Hash a = {};
  Hash b = {};
  EXPECT_EQ(a, b);
  EXPECT_EQ(b, a);

  b.qwords[0] = 1;
  EXPECT_EQ(b, b);
  EXPECT_NE(a, b);
  EXPECT_NE(b, a);
  EXPECT_LT(a, b);

  Hash c = {};
  c.qwords[1] = 2;
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
  EXPECT_LT(a, c);
}

// Check that hashes can be used with std::set. This requires comparison operators.
TEST(MetroHashTest, StdSet) {
  std::set<Hash> hashes;
  Hash a = {};
  hashes.insert(a);
  Hash b = {};
  b.dwords[1] = 5;
  hashes.insert(b);
  EXPECT_EQ(hashes.size(), 2u);
  Hash c = {};
  c.dwords[2] = 4;
  hashes.insert(c);
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));

  // Insert a duplicate value.
  hashes.insert({});
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));
}

// Check that hashes can be used with std::unordered_set. This requires std::hash implementation.
TEST(MetroHashTest, StdUnorderedSet) {
  std::unordered_set<Hash> hashes;
  Hash a = {};
  hashes.insert(a);
  Hash b = {};
  b.dwords[1] = 5;
  hashes.insert(b);
  EXPECT_EQ(hashes.size(), 2u);
  Hash c = {};
  c.dwords[2] = 4;
  hashes.insert(c);
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));

  // Insert a duplicate value.
  hashes.insert({});
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));
}

// Check that hashes can be used with llvm::DenseSet. This requires llvm::DenseMapInfo implementation.
TEST(MetroHashTest, ADTUnorderedSet) {
  DenseSet<Hash> hashes;
  Hash a = {};
  hashes.insert(a);
  Hash b = {};
  b.dwords[1] = 5;
  hashes.insert(b);
  EXPECT_EQ(hashes.size(), 2u);
  Hash c = {};
  c.dwords[2] = 4;
  hashes.insert(c);
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));

  // Insert a duplicate value.
  hashes.insert({});
  EXPECT_EQ(hashes.size(), 3u);
  EXPECT_THAT(hashes, UnorderedElementsAre(a, b, c));
}

} // namespace
} // namespace Llpc
