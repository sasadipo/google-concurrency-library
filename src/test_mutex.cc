#include "mutex.h"
#include "test_mutex.h"

#include <iostream>

using std::cerr;

namespace MutexInternal {

// Returns true if a thread id is null
static bool isNull(thread::id id) {
  return id == thread::id();
}

// Utility class that combines a mutex and a condition variable. Does
// not use the mutex defined in mutex.h because when this class is
// active, the debug mutex implementation is used.
class CompoundConditionVariable {
 public:
  CompoundConditionVariable()
      : locked_(false) {
    pthread_mutexattr_t mutex_attribute;
    handle_err_return(pthread_mutexattr_init(&mutex_attribute));
    handle_err_return(pthread_mutexattr_settype(&mutex_attribute,
                                                PTHREAD_MUTEX_DEFAULT));
    handle_err_return(pthread_mutex_init(&lock_, &mutex_attribute));
    handle_err_return(pthread_mutexattr_destroy(&mutex_attribute));
    handle_err_return(pthread_cond_init(&cond_, NULL));
  }

  ~CompoundConditionVariable() {
    handle_err_return(pthread_mutex_destroy(&lock_));
    handle_err_return(pthread_cond_destroy(&cond_));
  }

  void lock() {
    handle_err_return(pthread_mutex_lock(&lock_));
  }

  void unlock() {
    handle_err_return(pthread_mutex_unlock(&lock_));
  }

  void wait() {
    locked_ = true;
    while(locked_) {
      handle_err_return(pthread_cond_wait(&cond_, &lock_));
    }
  }

  void notify_all() {
    locked_ = false;
    handle_err_return(pthread_cond_broadcast(&cond_));
  }

  void notify_one() {
    locked_ = false;
    handle_err_return(pthread_cond_signal(&cond_));
  }

 private:
  bool locked_;
  pthread_mutex_t lock_;
  pthread_cond_t cond_;
};

// Utility class that locks a CompoundConditionVariable, and unlocks
// it when it goes out of scope.
class CompoundLockGuard {
 public:
  CompoundLockGuard(CompoundConditionVariable* cv)
      : cv_(cv) {
    cv_->lock();
  }
  ~CompoundLockGuard() {
    cv_->unlock();
  }
 private:
  CompoundConditionVariable* cv_;
};

// Internal struct used by test version of _posix_mutex
struct MutexMonitor {
  MutexMonitor(int mutex_type) : type(mutex_type) {
  }
  int type;
  thread::id locker;
  CompoundConditionVariable cv;
};

// Test implementation of _posix_mutex
_posix_mutex::_posix_mutex(int mutex_type) {
  handle_.monitor = new MutexMonitor(mutex_type);
}

_posix_mutex::~_posix_mutex() {
  delete monitor();
}

void _posix_mutex::lock() {
  CompoundLockGuard guard(&monitor()->cv);
  THREAD_DBG << "mutex locking " << this << " locked by "
             << monitor()->locker << ENDL;

  if (monitor()->type == PTHREAD_MUTEX_RECURSIVE) {
    if (monitor()->locker == this_thread::get_id()) {
      THREAD_DBG << "mutex : recursive lock" << ENDL;
      return;
    }
  }
  if (isNull(monitor()->locker)) {
    monitor()->locker = this_thread::get_id();
  } else {
    ThreadMonitor::GetInstance()->OnThreadBlocked(this_thread::get_id());
    THREAD_DBG << "mutex waiting on  " << this << " -> "
               << &monitor()->cv << ENDL;
    monitor()->cv.wait();
    THREAD_DBG << "mutex waited on  " << this << " -> "
               << &monitor()->cv << ENDL;
    ThreadMonitor::GetInstance()->OnThreadReleased(this_thread::get_id());
  }
  THREAD_DBG << "mutex : finished locking" << ENDL;
}

bool _posix_mutex::try_lock() {
  CompoundLockGuard guard(&monitor()->cv);
  if (isNull(monitor()->locker)) {
    monitor()->locker = this_thread::get_id();
    return true;
  }
  return false;
}

void _posix_mutex::unlock() {
  CompoundLockGuard guard(&monitor()->cv);
  THREAD_DBG << "mutex unlocking " << this << " locked by " << monitor()->locker
             << " in thread " << this_thread::get_id() << ENDL;
  monitor()->locker = thread::id();
  THREAD_DBG << "mutex notifying " << this << " locked by " << monitor()->locker
             << " -> " << &monitor()->cv << ENDL;

  monitor()->cv.notify_all();
}

static MutexInternal::CompoundConditionVariable gCv;

// Locks stderr and prints the thread id. Used by the THREAD_DBG
// macro.
extern void lock_stderr() {
  gCv.lock();
  cerr << this_thread::get_id() << " : ";
}

extern void unlock_stderr() {
  cerr.flush();
  gCv.unlock();
}
} // end namespace MutexInternal

using MutexInternal::CompoundLockGuard;

// The condition_variable's handle is declared as a union of a native
// pthread_cond_t and a ConditionMonitor*, where ConditionMonitor is
// declared but not defined. Here we define ConditionMonitor as a
// simple wrapper around the CompoundConditionVariable type. (We don't
// want to refer to the MutexInternal namespace in
// condition_variable.h, so we use the ConditionMonitor type.)
struct ConditionMonitor {
  MutexInternal::CompoundConditionVariable cv;
};

condition_variable::condition_variable() {
  handle_.monitor = new ConditionMonitor();
}

condition_variable::~condition_variable() {
  delete monitor();
}

void condition_variable::notify_one() {
  CompoundLockGuard guard(&monitor()->cv);
  monitor()->cv.notify_all();
}

void condition_variable::notify_all() {
  CompoundLockGuard guard(&monitor()->cv);
  monitor()->cv.notify_one();
}

void condition_variable::wait(unique_lock<mutex>& lock) {
  THREAD_DBG << "Cv wait " << ENDL;

  assert(lock.owns_lock());
  CompoundLockGuard guard(&monitor()->cv);
  lock.unlock();
  ThreadMonitor::GetInstance()->OnThreadBlocked(this_thread::get_id());
  THREAD_DBG << "Cv waiting on  " << this << " -> " << &monitor()->cv << ENDL;
  monitor()->cv.wait();
  THREAD_DBG << "Cv done waiting on  " << this << " -> "
             << &monitor()->cv << ENDL;

  lock.lock();
  ThreadMonitor::GetInstance()->OnThreadReleased(this_thread::get_id());
}

using MutexInternal::CompoundConditionVariable;
using MutexInternal::CompoundLockGuard;

// A simple latch used by ThreadMonitor. The ThreadMonitor maintains a
// map of thread id to the corresponding ThreadLatch. When a thread
// changes state, the corresponding latch can be notified.
class ThreadLatch {
 public:
  ThreadLatch() : blocked_(true) {
  }

  void wait() {
    CompoundLockGuard guard(&cv_);
    THREAD_DBG << "waiting for " << this << " blocked_ = " << blocked_ << ENDL;
    while(blocked_) {
      cv_.wait();
    }
    THREAD_DBG << "waited for " << this << " blocked_ = " << blocked_ << ENDL;
  }

  void notify () {
    CompoundLockGuard guard(&cv_);
    blocked_ = false;
    THREAD_DBG << "notifying  " << this << ENDL;
    cv_.notify_all();
  }

  void reset () {
    CompoundLockGuard guard(&cv_);
    blocked_ = true;
  }

 private:
  bool blocked_;
  CompoundConditionVariable cv_;
};

ThreadMonitor ThreadMonitor::gMonitor;

ThreadMonitor* ThreadMonitor::GetInstance() {
  return &gMonitor;
}

ThreadMonitor::ThreadMonitor()
  : map_lock_(new CompoundConditionVariable) {
}

ThreadMonitor::~ThreadMonitor() {
  delete map_lock_;
}

ThreadLatch* ThreadMonitor::GetThreadLatch(thread::id id) {
  assert(!MutexInternal::isNull(id));
  CompoundLockGuard guard(map_lock_);
  ThreadLatch* result;
  ThreadMap::iterator it = blocked_waiters_.find(id);
  if (it == blocked_waiters_.end()) {
    result = new ThreadLatch();
    blocked_waiters_[id] = result;
  } else {
    result = it->second;
  }
  THREAD_DBG << "latch for " << id << " = " << result << ENDL;
  return result;
}
void ThreadMonitor::WaitUntilBlocked(thread::id id) {
  ThreadLatch* latch = GetThreadLatch(id);
  latch->wait();
}

void ThreadMonitor::OnThreadBlocked(thread::id id) {
  ThreadLatch* latch = GetThreadLatch(id);
  latch->notify();
}

void ThreadMonitor::OnThreadReleased(thread::id id) {
  ThreadLatch* latch = GetThreadLatch(id);
  latch->reset();
}