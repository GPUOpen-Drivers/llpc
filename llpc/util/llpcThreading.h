/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  llpcThreading.h
 * @brief LLPC header file: contains the definition of LLPC multi-threading utilities
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace Llpc {

class IHelperThreadProvider;

/// The level of exclusion that is required for helper threads in @ref parallelForWithContext.
enum class HelperThreadExclusion {
  // No exclusion necessary.
  None,

  // The main thread must no longer be running with context == nullptr when the taskFunction is called for a helper
  // thread.
  Task,

  // In addition to @ref Task, the main thread also must not be running with context == nullptr when the createContext
  // function is called.
  CreateContext,
};

namespace detail {
// =====================================================================================================================
// Decides how many concurrent threads to use, taking into the requested number of threads, the number of tasks
// (work items), and the number of available CPU cores. The returned number is positive and not greater than the number
// of tasks.
//
// This is an implementation detail.
//
// @param numThreadsRequested : Number of requested threads. Pass 0 to indicate that all available cores are preferred.
// @param numTasks : Number of tasks (work items).
// @param numAvailableCores : Number of available processors.
// @returns : Number of concurrent threads to use. This will always be a positive number.
inline unsigned decideNumConcurrentThreads(size_t numThreadsRequested, size_t numTasks, size_t numAvailableCores) {
  if (numThreadsRequested == 1 || numTasks <= 1)
    return 1;

  if (numThreadsRequested == 0) {
    // Account for `hardware_concurrency` returning 0 in environments that do not allow querying this.
    return std::min(numTasks, std::max(numAvailableCores, size_t(1)));
  }

  return std::min(numThreadsRequested, numTasks);
}

llvm::Error parallelForWithContextImpl(size_t numExtraThreads, IHelperThreadProvider *helperThreadProvider,
                                       size_t numTasks, HelperThreadExclusion helperThreadExclusion,
                                       llvm::function_ref<void *()> createContext,
                                       llvm::function_ref<llvm::Error(size_t, void *)> taskFunction,
                                       llvm::function_ref<void(void *)> destroyContext);
} // namespace detail

// A parallel for loop using an optional IHelperThreadProvider and a given number of extra threads that are created
// on-the-fly.
//
// This function is designed for tasks where the helper threads require some context that is expensive to set up and/or
// running on a helper thread is less efficient, and the context can be re-used across individual tasks.
//
// The taskFunction is called for each task (as long as no error is encountered), with a context created by
// createContext as an argument or null if the task is called on the main thread (the thread calling
// parallelForWithContext).
//
// Set helperExclusion to a value other than @ref HelperThreadExclusion::None if helper tasks cannot run while
// taskFunction is running with a null context on the main thread. In that case, as soon as helper threads join,
// the main thread will create its own context to run subsequent tasks with.
//
// Returns the first error that was returned by taskFunction. Once an error is encountered, subsequent tasks may be
// skipped.
template <typename ContextT>
llvm::Error parallelForWithContext(size_t numExtraThreads, IHelperThreadProvider *helperThreadProvider, size_t numTasks,
                                   HelperThreadExclusion helperThreadExclusion,
                                   llvm::function_ref<std::unique_ptr<ContextT>()> createContext,
                                   llvm::function_ref<llvm::Error(size_t, ContextT *)> taskFunction,
                                   llvm::function_ref<void(std::unique_ptr<ContextT>)> destroyContext) {
  // Forward to a type-erased implementation. The type erasure costs a heap allocation of the context (instead of a
  // stack allocation on the helper thread stack), but the premise is that the context is expensive to create anyway.
  return ::Llpc::detail::parallelForWithContextImpl(
      numExtraThreads, helperThreadProvider, numTasks, helperThreadExclusion,
      [createContext]() -> void * {
        std::unique_ptr<ContextT> ctx = createContext();
        return ctx.release();
      },
      [taskFunction](size_t idx, void *context) -> llvm::Error {
        return taskFunction(idx, static_cast<ContextT *>(context));
      },
      [destroyContext](void *context) { destroyContext(std::unique_ptr<ContextT>(static_cast<ContextT *>(context))); });
}

// =====================================================================================================================
// A parallel for loop implementation using a simple worker thread pool. Unlike `llvm::parallel*` algorithms, does not
// depend on a global thread pool strategy.
//
// Applies the provided `function` to each input in `inputs`. This may happen parallel, depending on the number of
// threads used. Stops as soon as it encounters an error.
//
// @param numThreads : Number of requested threads. Pass 0 to indicate that all available cores are preferred.
//                     The implementation may spawn a different number of threads than requested to avoid unutilized
//                     threads.
// @param inputs : Random-access range with inputs that will be passed to `function`.
// @param function : Function object that will be applied to each input. Must return `llvm::Error`.
// @returns : `llvm::ErrorSuccess` on success, an error or combination on errors from `function` on failure.
template <typename RangeT, typename FuncT> llvm::Error parallelFor(size_t numThreads, RangeT &&inputs, FuncT function) {
  const auto inputsBegin = llvm::adl_begin(inputs);
  const auto inputsEnd = llvm::adl_end(inputs);
  const size_t numTasks = std::distance(inputsBegin, inputsEnd);
  const size_t numWorkers =
      detail::decideNumConcurrentThreads(numThreads, numTasks, std::thread::hardware_concurrency());

  // No need to spawn any threads if the work requires only one worker. This makes stack traces nicer.
  if (numWorkers == 1) {
    for (auto &&input : inputs)
      if (llvm::Error err = function(std::forward<decltype(input)>(input)))
        return err;

    return llvm::Error::success();
  }

  llvm::Error firstErr = llvm::Error::success();
  std::mutex failureMutex;

  std::vector<std::thread> workers(numWorkers);
  std::atomic<size_t> nextTaskIdx(0);

  for (std::thread &worker : workers) {
    worker = std::thread([&function, &firstErr, &failureMutex, &nextTaskIdx, numTasks, inputsBegin] {
      for (size_t pos = ++nextTaskIdx; pos <= numTasks; pos = ++nextTaskIdx) {
        const size_t idx = pos - 1;
        auto inputIt = inputsBegin + idx;
        if (llvm::Error err = function(*inputIt)) {
          nextTaskIdx = numTasks + 1; // Make the other threads finish without picking up any remaining tasks.
          std::lock_guard<std::mutex> lock(failureMutex);
          firstErr = llvm::joinErrors(std::move(firstErr), std::move(err));
          break;
        }
      }
    });
  }

  // Wait for all workers to finish.
  for (std::thread &worker : workers)
    worker.join();

  return firstErr;
}

} // namespace Llpc
