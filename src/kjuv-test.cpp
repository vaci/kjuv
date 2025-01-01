// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "kjuv.h"

#include <kj/main.h>

#include <gtest/gtest.h>

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

struct UvTest
  : testing::Test {

  UvTest() {
  }

  ~UvTest() noexcept {
  }

  uv_loop_t* uvLoop{uv_default_loop()};
  kj::UvEventPort eventPort{uvLoop};
  kj::EventLoop kjLoop{eventPort};
  kj::WaitScope waitScope{kjLoop};
};

TEST_F(UvTest, Basic) {
  auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
  fulfiller->fulfill();
  promise.wait(waitScope);
}

TEST_F(UvTest, Reject) {
  auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
  fulfiller->reject(KJ_EXCEPTION(FAILED));
  EXPECT_ANY_THROW(promise.wait(waitScope));
}

TEST_F(UvTest, Timer) {
  auto lowLevel = kj::newUvLowLevelAsyncIoProvider(eventPort);
  auto aio = kj::newAsyncIoProvider(*lowLevel);

  auto& timer = aio->getTimer();
  auto promise = timer.afterDelay(kj::NANOSECONDS*1);
  promise.wait(waitScope);
}

TEST_F(UvTest, TimerReschedule) {
  auto lowLevel = kj::newUvLowLevelAsyncIoProvider(eventPort);
  auto aio = kj::newAsyncIoProvider(*lowLevel);

  auto& timer = aio->getTimer();
  timer.afterDelay(kj::NANOSECONDS).then(
    [&timer]{
      return timer.afterDelay(kj::NANOSECONDS);
    }).wait(waitScope);
}

TEST_F(UvTest, CrossThreadFulfillerThisThread) {
  auto [promise, fulfiller] = kj::newPromiseAndCrossThreadFulfiller<void>();
  fulfiller->fulfill();
  promise.wait(waitScope);
}

TEST_F(UvTest, CrossThreadFulfillerOtherThread) {
  auto [promise, fulfiller] = kj::newPromiseAndCrossThreadFulfiller<void>();
  auto thread = kj::Thread([fulfiller = kj::mv(fulfiller)]{
    fulfiller->fulfill();
  });
  promise.wait(waitScope);
}

TEST_F(UvTest, PipeStream) {
  auto lowLevel = kj::newUvLowLevelAsyncIoProvider(eventPort);
  auto aio = kj::newAsyncIoProvider(*lowLevel);
  auto pipe = aio->newOneWayPipe();

  auto pth = aio->newPipeThread([](auto& aio, auto& stream, auto& waitScope) {
    constexpr auto txt = "hello"_kj;
    stream.write(txt.begin(), txt.size()).wait(waitScope);
  });

  auto txt = pth.pipe->readAllText().wait(waitScope);
  EXPECT_STREQ(txt.cStr(), "hello");
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
