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

#include "llpcError.h"
#include "llpcThreading.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <atomic>
#include <mutex>

using namespace llvm;

namespace Llpc {
namespace {

// cppcheck-suppress syntaxError
TEST(ThreadingTest, PlaceholderPass) {
  EXPECT_TRUE(true);
}

// cppcheck-suppress syntaxError
TEST(ThreadingTest, DecideNumConcurrentThreads) {
  using detail::decideNumConcurrentThreads;

  // Single thread requested.
  EXPECT_EQ(decideNumConcurrentThreads(1, 10, 8), 1u);

  // Two threads requested, multiple tasks.
  EXPECT_EQ(decideNumConcurrentThreads(2, 10, 8), 2u);

  // Two threads requested, single task.
  EXPECT_EQ(decideNumConcurrentThreads(2, 1, 8), 1u);

  // Three threads requested, two tasks.
  EXPECT_EQ(decideNumConcurrentThreads(3, 2, 8), 2u);

  // 'All CPUs' requested, multiple tasks.
  EXPECT_EQ(decideNumConcurrentThreads(0, 10, 8), 8u);

  // 'All CPUs' requested, few tasks.
  EXPECT_EQ(decideNumConcurrentThreads(0, 3, 8), 3u);

  // 'All CPUs' requested, but 0 logical processors.
  EXPECT_EQ(decideNumConcurrentThreads(0, 3, 0), 1u);
}

TEST(ThreadingTest, NoTasks) {
  const SmallVector<unsigned> data;

  for (size_t numThreads : {0, 1, 2, 16}) {
    std::atomic<unsigned> numExecutions(0);

    Error err = parallelFor(numThreads, data, [&numExecutions](unsigned datum) {
      (void)datum;
      ++numExecutions;
      return Error::success();
    });
    EXPECT_THAT_ERROR(std::move(err), Succeeded());
    EXPECT_EQ(numExecutions, 0u);
  }
}

TEST(ThreadingTest, OneTask) {
  const unsigned data[1] = {1};

  for (size_t numThreads : {0, 1, 2, 16}) {
    std::atomic<unsigned> numExecutions(0);
    std::atomic<unsigned> seenDatum(0);

    Error err = parallelFor(numThreads, data, [&numExecutions, &seenDatum](unsigned datum) {
      ++numExecutions;
      seenDatum = datum;
      return Error::success();
    });
    EXPECT_THAT_ERROR(std::move(err), Succeeded());
    EXPECT_EQ(numExecutions, 1u);
    EXPECT_EQ(seenDatum, 1u);
  }
}

TEST(ThreadingTest, MultipleTasks) {
  const auto data = seq(0u, 32u);

  for (size_t numThreads : {0, 1, 2, 7, 16}) {
    SmallVector<unsigned> seenNumbers;
    std::mutex seenMutex;

    Error err = parallelFor(numThreads, data, [&seenNumbers, &seenMutex](unsigned datum) {
      std::lock_guard<std::mutex> lock(seenMutex);
      seenNumbers.push_back(datum);
      return Error::success();
    });

    EXPECT_THAT_ERROR(std::move(err), Succeeded());
    EXPECT_EQ(seenNumbers.size(), data.size());
    EXPECT_TRUE(std::is_permutation(seenNumbers.begin(), seenNumbers.end(), data.begin(), data.end()));
  }
}

TEST(ThreadingTest, SingleError) {
  const auto data = seq(0u, 32u);

  for (size_t numThreads : {0, 1, 2, 7, 16}) {
    SmallVector<unsigned> seenNumbers;
    std::mutex seenMutex;

    // Make it fail after 13 iterations. The `err` result should be one or more errors joined together.
    Error err = parallelFor(numThreads, data, [&seenNumbers, &seenMutex](unsigned datum) -> Error {
      std::lock_guard<std::mutex> lock(seenMutex);
      if (seenNumbers.size() == 13)
        return createResultError(Vkgc::Result::Unsupported, "Unlucky");

      seenNumbers.push_back(datum);
      return Error::success();
    });

    using ::testing::AllOf;
    using ::testing::Each;
    using ::testing::Ge;
    using ::testing::HasSubstr;
    using ::testing::SizeIs;
    // Check that there is at least one error and that every error has the expected substring "Unlucky".
    EXPECT_THAT_ERROR(std::move(err), FailedWithMessageArray(AllOf(SizeIs(Ge(1u)), Each(HasSubstr("Unlucky")))));
    EXPECT_EQ(seenNumbers.size(), 13u);
    for (unsigned seen : seenNumbers)
      EXPECT_TRUE(is_contained(data, seen));
  }
}

} // namespace
} // namespace Llpc
