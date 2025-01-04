#pragma once

#include <kj/async.h>
#include <kj/async-io.h>
#include <kj/io.h>

#include <uv.h>

namespace kj {

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
  kj::EventLoop kjLoop;
  const kj::MonotonicClock& clock;
  TimerImpl timerImpl;

  uv_timer_t uvTimer;
  // fires when the next timer event is ready

  uv_timer_t uvWakeup;
  // fires when the KJ event loop is runnable
  
  kj::AutoCloseFd eventFd;
  uv_poll_t uvEventFdPoller;
  // cross-thread event

  bool woken = false;
  // true if a cross-thread event occurred

  void run();
  void scheduleTimers();

  static void doRun(uv_timer_t* handle);
  static void doTimer(uv_timer_t* timer);
  static void doEventFd(uv_poll_t* handle, int status, int events);

  friend class UvLowLevelAsyncIoProvider;
};

kj::Own<LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(UvEventPort& eventPort);

}
