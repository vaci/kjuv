#pragma once

#include <kj/async-io.h>
#include <kj/io.h>

#include <uv.h>

namespace kj {

class UvEventPort: public kj::EventPort {

  struct Impl;
  
 public:
  UvEventPort(uv_loop_t* loop);
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
  kj::Own<Impl> impl;

  friend class UvLowLevelAsyncIoProvider;
};

kj::Own<LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(UvEventPort& eventPort);

}
