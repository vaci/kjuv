#include "kjuv.h"

// Copyright (c) 2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Largely cribbed from
// https://github.com/capnproto/node-capnp/blob/node14/src/node-capnp/capnp.cc

#include <kj/async.h>
#include <kj/async-io.h>
#include <kj/io.h>
#include <kj/vector.h>

#include <errno.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace kj {

#define UV_CALL(code, ...)                \
  {                                             \
    auto result = (code);                                       \
    KJ_ASSERT(result == 0, uv_strerror(result), ##__VA_ARGS__); \
  }

namespace {

void setNonblocking(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFL));
  if ((flags & O_NONBLOCK) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
  }
}

void setCloseOnExec(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFD));
  if ((flags & FD_CLOEXEC) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFD, flags | FD_CLOEXEC));
  }
}

int applyFlags(int fd, uint flags) {
  if (flags & kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK) {
    KJ_DREQUIRE(
      fcntl(fd, F_GETFL) & O_NONBLOCK,
      "You claimed you set NONBLOCK, but you didn't.");
  }
  else {
    setNonblocking(fd);
  }

  if (flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) {
    if (flags & kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC) {
      KJ_DREQUIRE(
        fcntl(fd, F_GETFD) & FD_CLOEXEC,
        "You claimed you set CLOEXEC, but you didn't.");
    }
    else {
      setCloseOnExec(fd);
    }
  }

  return fd;
}

kj::AutoCloseFd openEventFd() {
  int fd;
  KJ_SYSCALL(fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  return kj::AutoCloseFd(fd);
}

}

UvEventPort::UvEventPort(uv_loop_t* loop, const kj::MonotonicClock& clock)
  : uvLoop{loop}
  , kjLoop{*this}
  , clock{clock}
  , timerImpl{clock.now()}
  , uvTimer{uv_timer_init, uvLoop}
  , uvRunnable{uv_idle_init, uvLoop}
  , uvWake{uv_async_init, uvLoop, &doWakeup} {

  uvTimer->data = this;
  uvRunnable->data = this;
  uvWake->data = this;
}

UvEventPort::~UvEventPort() {
  uv_timer_stop(uvTimer);
  uv_idle_stop(uvRunnable);
}

void UvEventPort::doRun(uv_idle_t* handle) {
  KJ_DASSERT(handle != nullptr);
  KJ_DASSERT(handle->data != nullptr);
  auto* self = reinterpret_cast<UvEventPort*>(handle->data);
  self->run();
}

void UvEventPort::run() {
  setRunnable(false);

  if (kjLoop.isRunnable()) {
    kjLoop.run();
  }

  if (kjLoop.isRunnable()) {
    // Apparently either we never became non-runnable,
    // or we did but then became runnable again.
    setRunnable(true);
  }
}

void UvEventPort::scheduleTimers() {
  auto timeout = timerImpl.timeoutToNextEvent(
    clock.now(), kj::MILLISECONDS, uint64_t(maxValue)
  ).orDefault(uint64_t(maxValue));
  UV_CALL(uv_timer_start(uvTimer, &doTimer, timeout, 0));
}

void UvEventPort::doTimer(uv_timer_t* handle)  {
  KJ_DASSERT(handle != nullptr);
  KJ_DASSERT(handle->data != nullptr);
  auto* self = reinterpret_cast<UvEventPort*>(handle->data);
  self->fireTimers();
}

void UvEventPort::fireTimers() {
  timerImpl.advanceTo(clock.now());
  scheduleTimers();
}

void UvEventPort::doWakeup(uv_async_t* handle)  {
  KJ_DASSERT(handle != nullptr);
  KJ_DASSERT(handle->data != nullptr);
  auto* self = reinterpret_cast<UvEventPort*>(handle->data);
  self->fireWakeup();
}

void UvEventPort::fireWakeup() {
  woken = true;
  setRunnable(true);
}

bool UvEventPort::wait() {
  scheduleTimers();
  uv_run(uvLoop, UV_RUN_ONCE);
  timerImpl.advanceTo(clock.now());

  if (woken) {
    woken = false;
    return true;
  }
  return false;
}

bool UvEventPort::poll() {
  scheduleTimers();
  UV_CALL(uv_run(uvLoop, UV_RUN_NOWAIT));
  timerImpl.advanceTo(clock.now());

  if (woken) {
    woken = false;
    return true;
  }
  return false;
}

void UvEventPort::wake() const {
  uv_async_send(uvWake);
}

void UvEventPort::setRunnable(bool runnable) {
  if (runnable) {
    UV_CALL(uv_idle_start(uvRunnable, &doRun));
  }
  else {
    UV_CALL(uv_idle_stop(uvRunnable));
  }
}

namespace {

static constexpr uint NEW_FD_FLAGS =
#if __linux__
  kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
  kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK |
#endif
  kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP;
// We always try to open FDs with CLOEXEC and NONBLOCK already set on Linux,
// but on other platforms this is not possible.

struct UvFileDescriptor {
  UvFileDescriptor(uv_loop_t* loop, int fd, uint flags)
    : uvLoop{loop}
    , fd{applyFlags(fd, flags)}
    , flags{flags}
    , uvPoller{uv_poll_init, uvLoop, fd} {
    uvPoller->data = this;
    UV_CALL(uv_poll_start(uvPoller, 0, &doPoll));
  }

  ~UvFileDescriptor() noexcept(false) {
    if (!stopped) {
      UV_CALL(uv_poll_stop(uvPoller));
    }

    // Don't use KJ_SYSCALL() here because close() should not be repeated on EINTR.
    if ((flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) && close(fd) < 0) {
      KJ_FAIL_SYSCALL("close", errno, fd) {
        // Recoverable exceptions are safe in destructors.
        break;
      }
    }
  }

  kj::Promise<void> onReadable() {
    if (stopped) {
      return kj::READY_NOW;
    }

    KJ_REQUIRE(
      readable == nullptr,
      "Must wait for previous event to complete.");

    auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
    readable = kj::mv(fulfiller);

    int flags = UV_READABLE | (writable == nullptr ? 0 : UV_WRITABLE);
    UV_CALL(uv_poll_start(uvPoller, flags, &doPoll));

    return kj::mv(promise);
  }

  kj::Promise<void> onWritable() {
    if (stopped) {
      return kj::READY_NOW;
    }

    KJ_REQUIRE(
      writable == nullptr,
      "Must wait for previous event to complete.");

    auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
    writable = kj::mv(fulfiller);

    int flags = UV_WRITABLE | (readable == nullptr ? 0 : UV_READABLE);
    UV_CALL(uv_poll_start(uvPoller, flags, &doPoll));

    return kj::mv(promise);
  }

protected:
  uv_loop_t* const uvLoop;
  const int fd;

private:
  uint flags;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> readable;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> writable;
  bool stopped = false;
  UvHandle<uv_poll_t> uvPoller;

  static void doPoll(uv_poll_t* handle, int status, int events) {
    KJ_DASSERT(handle != nullptr);
    KJ_DASSERT(handle->data != nullptr);
    auto* self = reinterpret_cast<UvFileDescriptor*>(handle->data);
    self->pollDone(status, events);
  }

  void pollDone(int status, int events) {
    if (KJ_UNLIKELY(status != 0)) {
      // Error.  libuv produces a non-zero status if polling produced POLLERR.  The error code
      // reported by libuv is always EBADF, even if the file descriptor is perfectly legitimate but
      // has simply become disconnected.  Instead of throwing an exception, we'd rather report
      // that the fd is now readable/writable and let the caller discover the error when they
      // actually attempt to read/write.
      KJ_IF_MAYBE(r, readable) {
        r->get()->fulfill();
        readable = nullptr;
      }

      KJ_IF_MAYBE(w, writable) {
        w->get()->fulfill();
        writable = nullptr;
      }

      // libuv automatically performs uv_poll_stop() before calling poll_cb with an error status.
      stopped = true;

    }
    else {
      // Fire the events.
      if (events & UV_READABLE) {
        KJ_ASSERT_NONNULL(readable)->fulfill();
        readable = nullptr;
      }
      if (events & UV_WRITABLE) {
        KJ_ASSERT_NONNULL(writable)->fulfill();
        writable = nullptr;
      }

      // Update the poll flags.
      int flags = (readable == nullptr ? 0 : UV_READABLE) |
                  (writable == nullptr ? 0 : UV_WRITABLE);
      UV_CALL(uv_poll_start(uvPoller, flags, &doPoll));
    }
  }
};

struct UvIoStream final: UvFileDescriptor, kj::AsyncIoStream {

  // IoStream implementation on top of libuv.  This is mostly a copy of the UnixEventPort-based
  // implementation in kj/async-io.c++.  We use uv_poll, which the libuv docs say is slow
  // "especially on Windows".  I'm guessing it's not so slow on Unix, since it matches the
  // underlying APIs.
  //
  // TODO(cleanup):  Allow better code sharing between the two.

  UvIoStream(uv_loop_t* loop, int fd, uint flags)
    : UvFileDescriptor{loop, fd, flags} {}

  virtual ~UvIoStream() noexcept(false) {}

  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    return tryReadInternal(buffer, minBytes, maxBytes, 0).then([=](size_t result) {
      KJ_REQUIRE(result >= minBytes, "Premature EOF") {
        // Pretend we read zeros from the input.
        memset(reinterpret_cast<byte*>(buffer) + result, 0, minBytes - result);
        return minBytes;
      }
      return result;
    });
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return tryReadInternal(buffer, minBytes, maxBytes, 0);
  }

  kj::Promise<void> write(const void* buffer, size_t size) override {
    ssize_t writeResult;
    KJ_NONBLOCKING_SYSCALL(writeResult = ::write(fd, buffer, size)) {
      return kj::READY_NOW;
    }

    // A negative result means EAGAIN, which we can treat the same as having written zero bytes.
    size_t n = writeResult < 0 ? 0 : writeResult;

    if (n == size) {
      return kj::READY_NOW;
    } else {
      buffer = reinterpret_cast<const byte*>(buffer) + n;
      size -= n;
    }

    return onWritable().then([this, buffer, size]() {
      return write(buffer, size);
    });
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    if (pieces.size() == 0) {
      return writeInternal(nullptr, nullptr);
    }
    else {
      return writeInternal(pieces[0], pieces.slice(1, pieces.size()));
    }
  }

  void shutdownWrite() override {
    // There's no legitimate way to get an AsyncStreamFd that isn't a socket through the
    // UnixAsyncIoProvider interface.
    KJ_SYSCALL(shutdown(fd, SHUT_WR));
  }

  kj::Promise<void> whenWriteDisconnected() override {
    // TODO(someday): Implement using UV_DISCONNECT?
    return kj::NEVER_DONE;
  }

private:
  kj::Promise<size_t> tryReadInternal(void* buffer, size_t minBytes, size_t maxBytes,
                                      size_t alreadyRead) {
    // `alreadyRead` is the number of bytes we have already received via previous reads -- minBytes,
    // maxBytes, and buffer have already been adjusted to account for them, but this count must
    // be included in the final return value.

    ssize_t n;
    KJ_NONBLOCKING_SYSCALL(n = ::read(fd, buffer, maxBytes)) {
      return alreadyRead;
    }

    if (n < 0) {
      // Read would block.
      return onReadable().then([this, buffer, minBytes, maxBytes, alreadyRead]() {
        return tryReadInternal(buffer, minBytes, maxBytes, alreadyRead);
      });
    }
    else if (n == 0) {
      // EOF -OR- maxBytes == 0.
      return alreadyRead;
    }
    else if (kj::implicitCast<size_t>(n) < minBytes) {
      // The kernel returned fewer bytes than we asked for (and fewer than we need).  This indicates
      // that we're out of data.  It could also mean we're at EOF.  We could check for EOF by doing
      // another read just to see if it returns zero, but that would mean making a redundant syscall
      // every time we receive a message on a long-lived connection.  So, instead, we optimistically
      // asume we are not at EOF and return to the event loop.
      //
      // If libuv provided notification of HUP or RDHUP, we could do better here...
      buffer = reinterpret_cast<byte*>(buffer) + n;
      minBytes -= n;
      maxBytes -= n;
      alreadyRead += n;
      return onReadable().then([this, buffer, minBytes, maxBytes, alreadyRead]() {
        return tryReadInternal(buffer, minBytes, maxBytes, alreadyRead);
      });
    }
    else {
      // We read enough to stop here.
      return alreadyRead + n;
    }
  }

  kj::Promise<void> writeInternal(
    kj::ArrayPtr<const byte> firstPiece,
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> morePieces) {

    KJ_STACK_ARRAY(struct iovec, iov, 1 + morePieces.size(), 16, 128);

    // writev() interface is not const-correct.  :(
    iov[0].iov_base = const_cast<byte*>(firstPiece.begin());
    iov[0].iov_len = firstPiece.size();
    for (uint i = 0; i < morePieces.size(); i++) {
      iov[i + 1].iov_base = const_cast<byte*>(morePieces[i].begin());
      iov[i + 1].iov_len = morePieces[i].size();
    }

    ssize_t writeResult;
    KJ_NONBLOCKING_SYSCALL(writeResult = ::writev(fd, iov.begin(), iov.size())) {
      // Error.

      // We can't "return kj::READY_NOW;" inside this block because it causes a memory leak due to
      // a bug that exists in both Clang and GCC:
      //   http://gcc.gnu.org/bugzilla/show_bug.cgi?id=33799
      //   http://llvm.org/bugs/show_bug.cgi?id=12286
      goto error;
    }
    if (false) {
    error:
      return kj::READY_NOW;
    }

    // A negative result means EAGAIN, which we can treat the same as having written zero bytes.
    size_t n = writeResult < 0 ? 0 : writeResult;

    // Discard all data that was written, then issue a new write for what's left (if any).
    for (;;) {
      if (n < firstPiece.size()) {
        // Only part of the first piece was consumed.  Wait for POLLOUT and then write again.
        firstPiece = firstPiece.slice(n, firstPiece.size());
        return onWritable().then([this, firstPiece, morePieces]() {
          return writeInternal(firstPiece, morePieces);
        });
      } else if (morePieces.size() == 0) {
        // First piece was fully-consumed and there are no more pieces, so we're done.
        KJ_DASSERT(n == firstPiece.size(), n);
        return kj::READY_NOW;
      } else {
        // First piece was fully consumed, so move on to the next piece.
        n -= firstPiece.size();
        firstPiece = morePieces[0];
        morePieces = morePieces.slice(1, morePieces.size());
      }
    }
  }
};

struct UvConnectionReceiver final: kj::ConnectionReceiver, UvFileDescriptor {
  // Like UvIoStream but for ConnectionReceiver.
  // This is also largely copied from kj/async-io.c++.

  UvConnectionReceiver(uv_loop_t* loop, int fd, uint flags)
      : UvFileDescriptor(loop, fd, flags) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    int newFd;

  retry:
#if __linux__
    newFd = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    newFd = ::accept(fd, nullptr, nullptr);
#endif

    if (newFd >= 0) {
      return kj::Own<kj::AsyncIoStream>(kj::heap<UvIoStream>(uvLoop, newFd, NEW_FD_FLAGS));
    }
    else {
      int error = errno;

      switch (error) {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
          // Not ready yet.
          return onReadable().then([this]() {
            return accept();
          });

        case EINTR:
        case ENETDOWN:
        case EPROTO:
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ECONNABORTED:
        case ETIMEDOUT:
          // According to the Linux man page, accept() may report an error
          // if the accepted connection is already broken.
          // In this case, we really ought to just ignore it and
          // keep waiting.  But it's hard to say exactly what errors
          // are such network errors and which ones are permanent errors.
          // We've made a guess here.
          goto retry;

        default:
          KJ_FAIL_SYSCALL("accept", error);
      }
    }
  }

  uint getPort() override {
    socklen_t addrlen;
    union {
      struct sockaddr generic;
      struct sockaddr_in inet4;
      struct sockaddr_in6 inet6;
    } addr;
    addrlen = sizeof(addr);
    KJ_SYSCALL(::getsockname(fd, &addr.generic, &addrlen));
    switch (addr.generic.sa_family) {
      case AF_INET: return ntohs(addr.inet4.sin_port);
      case AF_INET6: return ntohs(addr.inet6.sin6_port);
      default: return 0;
    }
  }
};

}

struct UvLowLevelAsyncIoProvider final: kj::LowLevelAsyncIoProvider {

  UvLowLevelAsyncIoProvider(UvEventPort& eventPort): eventPort(eventPort) {}

  kj::Own<kj::AsyncInputStream> wrapInputFd(int fd, uint flags = 0) override {
    return kj::heap<UvIoStream>(eventPort.getUvLoop(), fd, flags);
  }
  kj::Own<kj::AsyncOutputStream> wrapOutputFd(int fd, uint flags = 0) override {
    return kj::heap<UvIoStream>(eventPort.getUvLoop(), fd, flags);
  }
  kj::Own<kj::AsyncIoStream> wrapSocketFd(int fd, uint flags = 0) override {
    return kj::heap<UvIoStream>(eventPort.getUvLoop(), fd, flags);
  }
  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapConnectingSocketFd(
    int fd, const struct sockaddr* addr,
    uint addrlen, uint flags = 0) override {

    // Unfortunately connect() doesn't fit the mold of KJ_NONBLOCKING_SYSCALL,
    // since it indicates non-blocking using EINPROGRESS.
    for (;;) {
      if (::connect(fd, addr, addrlen) < 0) {
        int error = errno;
        if (error == EINPROGRESS) {
          // Fine.
          break;
        }
        else if (error != EINTR) {
          KJ_FAIL_SYSCALL("connect()", error) { break; }
          return kj::Own<kj::AsyncIoStream>();
        }
      }
      else {
        // no error
        break;
      }
    }

    auto stream = kj::heap<UvIoStream>(eventPort.getUvLoop(), fd, flags);
    auto connected = stream->onWritable();
    return connected.then([fd, stream = kj::mv(stream)]() mutable {
      int err;
      socklen_t errlen = sizeof(err);
      KJ_SYSCALL(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen));
      if (err != 0) {
        KJ_FAIL_SYSCALL("connect()", err) { break; }
      }
      return kj::Own<kj::AsyncIoStream>(kj::mv(stream));
    });
  }

  kj::Own<kj::ConnectionReceiver> wrapListenSocketFd(
    int fd,
    kj::LowLevelAsyncIoProvider::NetworkFilter& filter,
    uint flags = 0) override {

    // TODO(soon): TODO(security): Actually use `filter`.
    // Currently no API is exposed to set a filter so it's not important yet.
    return kj::heap<UvConnectionReceiver>(eventPort.getUvLoop(), fd, flags);
  }

  kj::Timer& getTimer() override {
    return eventPort.timerImpl;
  }

private:
  UvEventPort& eventPort;
};

kj::Own<kj::LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(
  kj::UvEventPort& eventPort) {
  return kj::heap<UvLowLevelAsyncIoProvider>(eventPort);
}

}

