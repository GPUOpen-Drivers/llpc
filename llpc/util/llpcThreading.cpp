/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llpcThreading.h"
#include "llpc.h"
#include <condition_variable>

using namespace llvm;
using namespace Llpc;

namespace {

// =====================================================================================================================
// Limited implementation of Llpc::IHelperThreadProvider to support extra threads when no helper thread provider is
// given.
class InternalHelperThreadProvider : public Llpc::IHelperThreadProvider {
public:
  virtual void SetTasks(ThreadFunction *, uint32_t numTasks, void *) override {
    assert(!m_totalInstances && "InternalHelperThreadProvider is single use");
    m_totalInstances = numTasks;
  }

  virtual bool GetNextTask(uint32_t *pTaskIndex) override {
    assert(pTaskIndex != nullptr);
    *pTaskIndex = m_nextInstance.fetch_add(1);
    return (*pTaskIndex < m_totalInstances);
  }

  virtual void TaskCompleted() override {
    uint32_t completedInstances = m_completedInstances.fetch_add(1) + 1;
    if (completedInstances == m_totalInstances)
      m_event.notify_all();
  }

  virtual void WaitForTasks() override {
    std::unique_lock<std::mutex> lock(m_lock);
    while (m_completedInstances < m_totalInstances)
      m_event.wait(lock);
  }

private:
  uint32_t m_totalInstances = 0;
  std::atomic<uint32_t> m_nextInstance = 0;
  std::atomic<uint32_t> m_completedInstances = 0;
  std::condition_variable m_event;
  std::mutex m_lock;
};

struct ParallelForWithContextState {
  std::atomic<bool> helperThreadJoined = false;
  std::atomic<bool> mainThreadUnlocked = false;
  std::mutex mutex;
  std::condition_variable_any cvar;
  std::atomic<bool> haveError = false;
  Error error = Error::success();
  HelperThreadExclusion helperThreadExclusion = HelperThreadExclusion::CreateContext;
  function_ref<void *()> createContext;
  function_ref<Error(size_t, void *)> taskFunction;
  function_ref<void(void *)> destroyContext;

  bool recordError(Error err) {
    // Record only the first error, ignore all subsequent ones.
    if (!haveError.exchange(true, std::memory_order_relaxed)) {
      // We have exclusive access here because
      //  1. the atomic exchange ensures that only one thread ever executes this assignment
      //  2. the error is read by the main thread only after waiting for all tasks to complete, and we only signal
      //     completion of the failed task after recording the error
      // The second point is also required to justify using a relaxed atomic for the exchange.
      error = std::move(err);
      return true;
    } else {
      consumeError(std::move(err));
      return false;
    }
  }

  // Returns true if all tasks are known to be completed or about to be completed by another thread.
  bool runInnerLoop(IHelperThreadProvider *helperThreadProvider, void *context, unsigned firstIndex,
                    function_ref<bool()> shouldBreak = {}) {
    unsigned taskIndex = firstIndex;
    do {
      bool error = false;
      bool recordedError = false;

      if (Error err = taskFunction(taskIndex, context)) {
        error = true;
        recordedError = recordError(std::move(err));
      }

      // Subtle: signaling completion must happen after recording an error.
      helperThreadProvider->TaskCompleted();

      if (recordedError) {
        // Drain all remaining tasks from a single thread when an error occurs.
        while (helperThreadProvider->GetNextTask(&taskIndex))
          helperThreadProvider->TaskCompleted();
      }

      if (error)
        return true; // either we just drained everything or somebody else does so concurrently

      if (shouldBreak && shouldBreak())
        return false;

      if (haveError.load(std::memory_order_relaxed)) {
        // Some other thread encountered an error and is draining all remaining tasks concurrently.
        return true;
      }
    } while (helperThreadProvider->GetNextTask(&taskIndex));

    return true;
  }

  // Entry point for helper thread that join the parallel for.
  static void runHelperThread(IHelperThreadProvider *helperThreadProvider, void *data) {
    ParallelForWithContextState *state = static_cast<ParallelForWithContextState *>(data);

    // Pre-load the flag to avoid dirtying shared data.
    if (!state->helperThreadJoined.load(std::memory_order_relaxed))
      state->helperThreadJoined.store(true, std::memory_order_relaxed);

    unsigned taskIndex;
    if (!helperThreadProvider->GetNextTask(&taskIndex))
      return;

    void *context = nullptr;

    if (state->helperThreadExclusion != HelperThreadExclusion::CreateContext) {
      // Create the context early if allowed so that we spend less time waiting for the main thread to unlock us.
      context = state->createContext();
    }

    if (state->helperThreadExclusion != HelperThreadExclusion::None &&
        !state->mainThreadUnlocked.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cvar.wait(state->mutex, [state]() { return state->mainThreadUnlocked.load(std::memory_order_acquire); });
    }

    if (!context)
      context = state->createContext();

    state->runInnerLoop(helperThreadProvider, context, taskIndex);
    state->destroyContext(context);
  };
};

} // anonymous namespace

Error Llpc::detail::parallelForWithContextImpl(size_t numExtraThreads, IHelperThreadProvider *helperThreadProvider,
                                               size_t numTasks, HelperThreadExclusion helperThreadExclusion,
                                               function_ref<void *()> createContext,
                                               function_ref<Error(size_t, void *)> taskFunction,
                                               function_ref<void(void *)> destroyContext) {
  if (!numTasks)
    return Error::success();

  InternalHelperThreadProvider ourHelperThreadProvider;
  if (numExtraThreads && !helperThreadProvider)
    helperThreadProvider = &ourHelperThreadProvider;

  if (!helperThreadProvider) {
    for (size_t i = 0; i < numTasks; ++i) {
      if (Error err = taskFunction(i, nullptr))
        return err;
    }
    return Error::success();
  }

  ParallelForWithContextState state;
  state.helperThreadExclusion = helperThreadExclusion;
  state.createContext = createContext;
  state.taskFunction = taskFunction;
  state.destroyContext = destroyContext;

  // If we have extra threads, assume that they join immediately so that we never give the exclusive lock to the main
  // thread.
  if (numExtraThreads) {
    state.helperThreadJoined.store(true, std::memory_order_relaxed);
    state.mainThreadUnlocked.store(true, std::memory_order_relaxed);
  }

  // This is implicitly a release fence. Helper threads may be executing from this point on.
  helperThreadProvider->SetTasks(&ParallelForWithContextState::runHelperThread, numTasks, &state);

  std::vector<std::thread> workers(numExtraThreads);
  for (std::thread &worker : workers) {
    worker = std::thread(
        [helperThreadProvider, &state] { ParallelForWithContextState::runHelperThread(helperThreadProvider, &state); });
  }

  unsigned taskIndex;
  if (!helperThreadProvider->GetNextTask(&taskIndex)) {
    // This can happen if a helper thread races us.
  } else {
    if (helperThreadExclusion == HelperThreadExclusion::None) {
      state.runInnerLoop(helperThreadProvider, nullptr, taskIndex);
    } else {
      bool drained = false;

      if (!numExtraThreads) {
        // If we don't spawn additional threads ourselves, we rely on threads from the provider. There is no guarantee
        // that other threads will arrive soon or at all, so run without a context on the main thread first. This avoids
        // the cost of running with a context if it later turns out to have been unnecessary.
        drained = state.runInnerLoop(helperThreadProvider, nullptr, taskIndex,
                                     [&state] { return state.helperThreadJoined.load(std::memory_order_relaxed); });
        if (!drained)
          drained = !helperThreadProvider->GetNextTask(&taskIndex);

        // The release pairs with the acquire in the helper thread. The point of this synchronization is to synchronize
        // the structures the caller has which require the helper thread exclusion. (We need it at least for the acquire
        // that happens outside of the mutex.)
        //
        // Note that if we have extra threads, we start in the unlocked state and don't have to notify the condition
        // variable.
        state.mainThreadUnlocked.store(true, std::memory_order_release);
        state.cvar.notify_all();
      }

      if (!drained) {
        void *context = state.createContext();
        state.runInnerLoop(helperThreadProvider, context, taskIndex);
        state.destroyContext(context);
      }
    }
  }

  helperThreadProvider->WaitForTasks();

  for (std::thread &worker : workers)
    worker.join();

  return std::move(state.error);
}
