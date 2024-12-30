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
};

TEST_F(UvTest, Basic) {
  auto* uvLoop = uv_default_loop();
  kj::UvEventPort eventPort{uvLoop};
  kj::EventLoop kjLoop{eventPort};
  kj::WaitScope waitScope{kjLoop};

  auto lowLevel = kj::newUvLowLevelAsyncIoProvider(eventPort);
  auto aio = kj::newAsyncIoProvider(*lowLevel);
}

TEST_F(UvTest, CrossThreadFulfiller) {
  auto* uvLoop = uv_default_loop();
  kj::UvEventPort eventPort{uvLoop};
  kj::EventLoop kjLoop{eventPort};
  kj::WaitScope waitScope{kjLoop};

  auto [promise, fulfiller] = kj::newPromiseAndCrossThreadFulfiller<void>();

  auto thread = kj::Thread([fulfiller = kj::mv(fulfiller)]{
    KJ_LOG(ERROR, "Fulfilling from thread");
    fulfiller->fulfill();
  });
  
  promise.wait(waitScope);
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
