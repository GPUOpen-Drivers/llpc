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

#include "llpc.h"
#include "llpcError.h"
#include "llpcShaderCache.h"
#include "llpcThreading.h"
#include "vkgcDefs.h"
#include "vkgcMetroHash.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <random>

using namespace llvm;
using ::testing::ElementsAreArray;

namespace Llpc {
namespace {

constexpr Vkgc::GfxIpVersion GfxIp = {10, 1, 0};

// Test class for tests that initialize a runtime ShaderCache.
class ShaderCacheTest : public ::testing::Test {
public:
  // Creates a new empty runtime ShaderCache.
  void SetUp() override {
    m_auxCreateInfo.shaderCacheMode = ShaderCacheMode::ShaderCacheEnableRuntime;
    m_auxCreateInfo.gfxIp = GfxIp;

    Result result = m_cache.init(&m_llpcCacheCreateInfo, &m_auxCreateInfo);
    EXPECT_EQ(result, Result::Success);
  }

  // Returns the ShaderCache object.
  ShaderCache &getCache() { return m_cache; }

  // Creates a hash from four dwords.
  static MetroHash::Hash hashFromDWords(unsigned a, unsigned b, unsigned c, unsigned d) {
    MetroHash::Hash hash = {};
    hash.dwords[0] = a;
    hash.dwords[1] = b;
    hash.dwords[2] = c;
    hash.dwords[3] = d;
    return hash;
  }

  // Creates an ArrayRef from the given blob and its size.
  static ArrayRef<char> charArrayFromBlob(const void *blob, size_t size) {
    return {reinterpret_cast<const char *>(blob), size};
  }

private:
  ShaderCache m_cache;
  ShaderCacheCreateInfo m_llpcCacheCreateInfo = {};
  ShaderCacheAuxCreateInfo m_auxCreateInfo = {};
};

// cppcheck-suppress syntaxError
TEST_F(ShaderCacheTest, CreateEmpty) {
  ShaderCache &cache = getCache();
  size_t size = 0;
  Result result = cache.Serialize(nullptr, &size);
  EXPECT_EQ(result, Result::Success);
  EXPECT_EQ(size, sizeof(ShaderCacheSerializedHeader));
}

TEST_F(ShaderCacheTest, InsertOne) {
  ShaderCache &cache = getCache();
  const auto hash = hashFromDWords(1, 2, 3, 4);
  SmallVector<char> cacheEntry(64);
  std::iota(cacheEntry.begin(), cacheEntry.end(), 0);

  CacheEntryHandle handle = nullptr;
  // Check if the entry is inside without allocating on miss.
  ShaderEntryState state = cache.findShader(hash, false, &handle);
  EXPECT_EQ(state, ShaderEntryState::Unavailable);
  EXPECT_EQ(handle, nullptr);

  // Check again but allocate this time.
  state = cache.findShader(hash, true, &handle);
  EXPECT_EQ(state, ShaderEntryState::Compiling);
  EXPECT_NE(handle, nullptr);

  // Insert the new entry.
  cache.insertShader(handle, cacheEntry.data(), cacheEntry.size());

  // The entry should be in the cache now.
  CacheEntryHandle newHandle = nullptr;
  state = cache.findShader(hash, false, &newHandle);
  EXPECT_EQ(state, ShaderEntryState::Ready);
  EXPECT_EQ(handle, newHandle);

  // Make sure that the content matches the inserted entry.
  const void *blob = nullptr;
  size_t blobSize = 0;
  Result result = cache.retrieveShader(handle, &blob, &blobSize);
  EXPECT_EQ(result, Result::Success);
  EXPECT_EQ(blobSize, cacheEntry.size());
  EXPECT_NE(blob, nullptr);
  EXPECT_THAT(charArrayFromBlob(blob, blobSize), ElementsAreArray(cacheEntry));

  size_t cacheSize = 0;
  result = cache.Serialize(nullptr, &cacheSize);
  EXPECT_EQ(result, Result::Success);
  EXPECT_GE(cacheSize, sizeof(ShaderCacheSerializedHeader) + blobSize);
}

TEST_F(ShaderCacheTest, InsertsShaders) {
  ShaderCache &cache = getCache();
  SmallVector<char> cacheEntry(64);
  constexpr size_t numShaders = 128;
  SmallVector<MetroHash::Hash, 0> hashes(numShaders);
  for (auto [idx, hash] : enumerate(hashes))
    hash = hashFromDWords(static_cast<unsigned>(idx), 2, 3, 4);

  for (auto &hash : hashes) {
    CacheEntryHandle handle = nullptr;
    // Check if the entry is inside without allocating on miss.
    ShaderEntryState state = cache.findShader(hash, false, &handle);
    EXPECT_EQ(state, ShaderEntryState::Unavailable);
    EXPECT_EQ(handle, nullptr);

    // Check again but allocate this time.
    state = cache.findShader(hash, true, &handle);
    EXPECT_EQ(state, ShaderEntryState::Compiling);
    EXPECT_NE(handle, nullptr);

    // Insert the new entry.
    cache.insertShader(handle, cacheEntry.data(), cacheEntry.size());
  }

  // All entries should be in the cache now.
  for (auto &hash : hashes) {
    CacheEntryHandle newHandle = nullptr;
    ShaderEntryState state = cache.findShader(hash, false, &newHandle);
    EXPECT_EQ(state, ShaderEntryState::Ready);
    EXPECT_NE(newHandle, nullptr);
  }

  size_t cacheSize = 0;
  Result result = cache.Serialize(nullptr, &cacheSize);
  EXPECT_EQ(result, Result::Success);
  EXPECT_GE(cacheSize, sizeof(ShaderCacheSerializedHeader) + (numShaders * cacheEntry.size()));
}

// This test tries to insert the same shader with N threads. We expect to see one insertion
// and N - 1 hits, for each shader.
// Disable it can fail or hang with the wait time in `ShaderCache::findShader` changed.
TEST_F(ShaderCacheTest, InsertsShadersMultithreaded) {
  ShaderCache &cache = getCache();
  SmallVector<char> cacheEntry(64);
  constexpr size_t numShaders = 128;
  constexpr size_t numThreads = 8;
  constexpr unsigned maxWaitTimeMilliseconds = 4;

  SmallVector<MetroHash::Hash, 0> hashes(numShaders);
  for (auto [idx, hash] : enumerate(hashes))
    hash = hashFromDWords(static_cast<unsigned>(idx), 2, 3, 4);

  // Initialize the generator with a deterministic seed.
  std::mt19937 generator(std::random_device{}());
  std::uniform_int_distribution<unsigned> waitTimeDistribution(0, maxWaitTimeMilliseconds * 1000);
  auto getWaitTime = [&generator, &waitTimeDistribution] {
    return std::chrono::microseconds(waitTimeDistribution(generator));
  };

  for (auto &hash : hashes) {
    std::atomic<size_t> numInsertions{0};
    std::atomic<size_t> numHits{0};

    Error err = parallelFor(numThreads, seq(size_t(0), numThreads),
                            [&cache, &cacheEntry, hash, &getWaitTime, &numInsertions, &numHits](size_t) -> Error {
                              CacheEntryHandle handle = nullptr;
                              ShaderEntryState state = cache.findShader(hash, true, &handle);
                              if (!handle)
                                return createResultError(Result::ErrorUnavailable);

                              if (state == ShaderEntryState::Compiling) {
                                // Insert the new entry. Sleep to simulate compilation time.
                                std::this_thread::sleep_for(getWaitTime());
                                cache.insertShader(handle, cacheEntry.data(), cacheEntry.size());
                                ++numInsertions;
                              } else {
                                EXPECT_EQ(state, ShaderEntryState::Ready);
                                ++numHits;
                              }
                              return Error::success();
                            });

    EXPECT_THAT_ERROR(std::move(err), Succeeded());
    EXPECT_EQ(numInsertions, 1);
    EXPECT_EQ(numHits, numThreads - 1);
  }

  // All entries should be in the cache now.
  for (auto &hash : hashes) {
    CacheEntryHandle newHandle = nullptr;
    ShaderEntryState state = cache.findShader(hash, false, &newHandle);
    EXPECT_EQ(state, ShaderEntryState::Ready);
    EXPECT_NE(newHandle, nullptr);
  }

  size_t cacheSize = 0;
  Result result = cache.Serialize(nullptr, &cacheSize);
  EXPECT_EQ(result, Result::Success);
  EXPECT_GE(cacheSize, sizeof(ShaderCacheSerializedHeader) + (numShaders * cacheEntry.size()));
}

} // namespace
} // namespace Llpc
