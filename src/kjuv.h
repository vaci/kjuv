#pragma once

#include <kj/async.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/io.h>

#include <uv.h>

namespace kj {

#define UV_CALL(code, loop, ...)                \
  {                                             \
    auto result = code;                                         \
    KJ_ASSERT(result == 0, uv_strerror(result), ##__VA_ARGS__); \
  }

class UvEventPort: public kj::EventPort {

 public:
  UvEventPort(uv_loop_t* loop);
  ~UvEventPort();
 
  kj::EventLoop& getKjLoop() { return kjLoop; }
  uv_loop_t* getUvLoop() { return loop; }

  bool wait() override {
    //UV_CALL(uv_run(loop, UV_RUN_ONCE), loop);
    uv_run(loop, UV_RUN_ONCE);

    // TODO(someday): Implement cross-thread wakeup.
    return false;
  }

  bool poll() override {
    UV_CALL(uv_run(loop, UV_RUN_NOWAIT), loop);

    // TODO(someday): Implement cross-thread wakeup.
    return false;
  }

  void wake() const override {
    KJ_LOG(ERROR, "waking");
    uint64_t one = 1;
    ssize_t n;
    KJ_NONBLOCKING_SYSCALL(n = write(eventFd, &one, sizeof(one)));
    KJ_ASSERT(n < 0 || n == sizeof(one));
  }

  void setRunnable(bool runnable) override {
    KJ_LOG(ERROR, "setRunnable", runnable, kjLoop.isRunnable());
    if (runnable != this->runnable) {
      this->runnable = runnable;
      if (runnable && !scheduled) {
        KJ_LOG(ERROR, "setRunnable: scheduling", runnable, scheduled);
        schedule();
      }
    }
  }

private:
  uv_loop_t* loop;
  uv_timer_t timer;
  kj::EventLoop kjLoop;
  bool runnable = false;
  bool scheduled = false;

  kj::AutoCloseFd eventFd;
  uv_poll_t eventHandle;

  void schedule() {
    KJ_LOG(ERROR, "Scheduled");
    scheduled = true;
    UV_CALL(uv_timer_start(&timer, &doRun, 0, 0), loop);
  }

  void run();

  static void doRun(uv_timer_t* handle) {
    KJ_ASSERT(handle != nullptr);
    UvEventPort* self = reinterpret_cast<UvEventPort*>(handle->data);
    self->run();
  }

  static void doEventFd(uv_poll_t* handle, int status, int events);
};

kj::Own<LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(UvEventPort& eventPort);

//kj::Own<kj::AsyncIoProvider> newAsyncIoProvider(kj::LowLevelAsyncIoProvider& lowLevel);
}
