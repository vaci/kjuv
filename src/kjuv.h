#pragma once

#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/io.h>

#include <uv.h>

namespace kj {

template <typename HandleType>
class UvHandle {
  // Encapsulates libuv handle lifetime into C++ object lifetime. This turns out to be hard.
  // If the loop is no longer running, memory will leak.
  //
  // Use like:
  //   UvHandle<uv_timer_t> timer(uv_timer_init, loop);
  //   uv_timer_start(timer, &callback, 0, 0);

public:
  template <typename Init, typename... Args>
  UvHandle(Init&& func, uv_loop_t* loop, Args&&... args)
    : handle{new HandleType} {

    auto result = kj::fwd<Init>(func)(loop, handle, kj::fwd<Args>(args)...);
    if (result < 0) {
      delete handle;
      auto error = uv_strerror(result);
      KJ_FAIL_ASSERT("creating UV handle failed", error);
    }
  }

  ~UvHandle() {
    uv_close(reinterpret_cast<uv_handle_t*>(handle), &closeCallback);
  }

  inline HandleType* operator->() { return handle; }
  inline operator HandleType*() { return handle; }
  inline operator uv_handle_t*() { return reinterpret_cast<uv_handle_t*>(handle); }

private:
  HandleType* handle;

  static void closeCallback(uv_handle_t* handle) {
    delete reinterpret_cast<HandleType*>(handle);
  }
};

class UvEventPort
  : public kj::EventPort {

 public:
  UvEventPort(
    uv_loop_t* loop,
    const kj::MonotonicClock& = kj::systemPreciseMonotonicClock());

  ~UvEventPort();

  kj::EventLoop& getKjLoop() { return kjLoop; }
  uv_loop_t* getUvLoop() { return uvLoop; }

  bool wait() override;
  bool poll() override;
  void wake() const override;
  void setRunnable(bool runnable) override;

private:
  uv_loop_t* uvLoop;
  kj::EventLoop kjLoop;

  static void doRun(uv_idle_t* handle);
  void run();

  void scheduleTimers();
  static void doTimer(uv_timer_t* timer);
  void fireTimers();

  static void doWakeup(uv_async_t* handle);
  void fireWakeup();

  const kj::MonotonicClock& clock;
  TimerImpl timerImpl;

  UvHandle<uv_timer_t> uvTimer;
  // fires when the next timer event is ready

  UvHandle<uv_idle_t> uvRunnable;
  // fires when the KJ event loop is to be run

  mutable UvHandle<uv_async_t> uvWake;
  // cross-thread event

  bool woken = false;
  // true if a cross-thread event occurred

  friend class UvLowLevelAsyncIoProvider;
};

kj::Own<LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(
  UvEventPort& eventPort);

}
