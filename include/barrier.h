// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GCL_BARRIER_
#define GCL_BARRIER_

#include <stddef.h>
#include <stdexcept>

#include <condition_variable.h>
#include <latch_base.h>
#include <mutex.h>

#if defined(__GXX_EXPERIMENTAL_CXX0X__)
#include <functional>
#else
#include <tr1/functional>
#endif

namespace gcl {
#if defined(__GXX_EXPERIMENTAL_CXX0X__)
using std::function;
#else
using std::tr1::function;
#endif

// Allows a set of threads to wait until all threads have reached a
// common point.
class barrier {
 public:
  // Creates a new barrier that will block until num_threads threads are waiting
  // on it. When the barrier is released, it will be reset, so that it can be
  // used again.
  // Throws invalid_argument if num_threads == 0.
  explicit barrier(size_t num_threads) throw (std::invalid_argument);

  // Creates a new barrier that will block until num_threads threads
  // are waiting on it. When the barrier is released, completion_fn will
  // be invoked.
  // Throws invalid_argument if num_threads == 0 or completion_fn is NULL.
  barrier(size_t num_threads, function<void()> completion_fn)
      throw (std::invalid_argument);

  ~barrier();

  // Blocks until num_threads have call count_down_and_wait(). Before releasing
  // any thread, invokes the completion function specified in the constructor.
  // If no completion function is registered, resets itself with the original
  // num_threads count.
  //
  // Memory ordering: For threads X and Y that call await(), the call
  // to await() in X happens before the return from await() in Y.
  void count_down_and_wait() throw (std::logic_error);

  // Resets the barrier with the specified number of threads. This method should
  // only be invoked when there are no other threads currently inside the wait()
  // method. It is also safe to invoke this method from within
  // count_down_and_wait(), via the registered completion_fn.
  void reset(size_t num_threads);

  // Resets the barrier with the specified completion_fn.  This method should
  // only be invoked when there are no other threads currently inside the wait()
  // method. It is also safe to invoke this method from within
  // count_down_and_wait(), via the registered completion_fn.
  //
  // If completion_fn is NULL, then the barrier behaves as if no completion_fn
  // were registered in the constructor.
  void reset(function<void()> completion_fn);

 private:
  void on_countdown();

  size_t thread_count_;
  latch_base latch1_;
  latch_base latch2_;
  latch_base* current_latch_;
  function<void()> completion_fn_;

  int new_count_;
  function<void()> new_completion_fn_;

};
}
#endif // GCL_BARRIER_
