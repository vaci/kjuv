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

  bool wait() override;
  bool poll() override;
  void wake() const override;
  void setRunnable(bool runnable) override;

private:
  uv_loop_t* loop;
  uv_timer_t uvScheduleTimer;

  uv_timer_t uvTimer;
  
  kj::EventLoop kjLoop;
  bool runnable = false;
  bool scheduled = false;

  kj::AutoCloseFd eventFd;
  uv_poll_t eventHandle;

  const kj::MonotonicClock& clock{kj::systemPreciseMonotonicClock()};
  TimerImpl timerImpl{clock.now()};

  void schedule();
  void run();
  void timeout();

  static void doRun(uv_timer_t* handle);
  static void doTimer(uv_timer_t* timer);
  static void doEventFd(uv_poll_t* handle, int status, int events);

  friend class UvLowLevelAsyncIoProvider;
};

kj::Own<LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(UvEventPort& eventPort);

//kj::Own<kj::AsyncIoProvider> newAsyncIoProvider(kj::LowLevelAsyncIoProvider& lowLevel);
}
