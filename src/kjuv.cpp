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

#if __cplusplus >= 201300
// Hack around stdlib bug with C++14.
#include <initializer_list>  // force libstdc++ to include its config
#undef _GLIBCXX_HAVE_GETS    // correct broken config
// End hack.
#endif

#include <capnp/dynamic.h>
#include <capnp/schema-parser.h>
#include <kj/debug.h>
#include <kj/async.h>
#include <kj/async-io.h>
#include <kj/io.h>
#include <kj/vector.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <set>
#include <typeinfo>
#include <typeindex>
#include <unordered_map>

namespace kj {

// =======================================================================================
// KJ <-> libuv glue.


template <typename HandleType>
class UvHandle {
  // Encapsulates libuv handle lifetime into C++ object lifetime. This turns out to be hard.
  // If the loop is no longer running, memory will leak.
  //
  // Use like:
  //   UvHandle<uv_timer_t> timer(uv_timer_init, loop);
  //   uv_timer_start(timer, &callback, 0, 0);

public:
  template <typename ConstructorFunc, typename... Args>
  UvHandle(ConstructorFunc&& func, uv_loop_t* loop, Args&&... args): handle(new HandleType) {
    auto result = func(loop, handle, kj::fwd<Args>(args)...);
    if (result < 0) {
      delete handle;
      auto error = uv_strerror(result);
      KJ_FAIL_ASSERT("creating UV handle failed", error);
    }
  }

  ~UvHandle() {
    uv_close(getBase(), &closeCallback);
  }

  inline HandleType& operator*() { return *handle; }
  inline const HandleType& operator*() const { return *handle; }

  inline HandleType* operator->() { return handle; }
  inline HandleType* operator->() const { return handle; }

  inline operator HandleType*() { return handle; }
  inline operator const HandleType*() const { return handle; }

  inline operator uv_handle_t*() { return reinterpret_cast<uv_handle_t*>(handle); }
  inline operator const uv_handle_t*() const { return reinterpret_cast<uv_handle_t*>(handle); }

  inline HandleType* get() { return handle; }
  inline HandleType* get() const { return handle; }

  inline uv_handle_t* getBase() { return reinterpret_cast<uv_handle_t*>(handle); }
  inline uv_handle_t* getBase() const { return reinterpret_cast<uv_handle_t*>(handle); }

private:
  HandleType* handle;

  static void closeCallback(uv_handle_t* handle) {
    delete reinterpret_cast<HandleType*>(handle);
  }
};


static void setNonblocking(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFL));
  if ((flags & O_NONBLOCK) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
  }
}

static void setCloseOnExec(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFD));
  if ((flags & FD_CLOEXEC) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFD, flags | FD_CLOEXEC));
  }
}

static int applyFlags(int fd, uint flags) {
  if (flags & kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK) {
    KJ_DREQUIRE(fcntl(fd, F_GETFL) & O_NONBLOCK, "You claimed you set NONBLOCK, but you didn't.");
  } else {
    setNonblocking(fd);
  }

  if (flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) {
    if (flags & kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC) {
      KJ_DREQUIRE(fcntl(fd, F_GETFD) & FD_CLOEXEC,
                  "You claimed you set CLOEXEC, but you didn't.");
    } else {
      setCloseOnExec(fd);
    }
  }

  return fd;
}

void UvEventPort::doEventFd(uv_poll_t* handle, int status, int events)  {
  KJ_LOG(ERROR, "doEventFd");
  UvEventPort* self = reinterpret_cast<UvEventPort*>(handle->data);
  uint64_t value;
  ssize_t n;
  KJ_NONBLOCKING_SYSCALL(n = read(self->eventFd, &value, sizeof(value)));
  KJ_ASSERT(n < 0 || n == sizeof(value));
  KJ_LOG(ERROR, "doEventFd setting runnable=true");
  self->setRunnable(true);
}

UvEventPort::UvEventPort(uv_loop_t* loop)
  : loop(loop),
    kjLoop(*this) {
  uv_timer_init(loop, &timer);
  timer.data = this;

  int fd;
  KJ_SYSCALL(fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  eventFd = AutoCloseFd(fd);

  uv_poll_init(loop, &eventHandle, eventFd);
  eventHandle.data = this;
  UV_CALL(uv_poll_start(&eventHandle, UV_READABLE, &doEventFd), loop);
}

UvEventPort::~UvEventPort() {
  if (scheduled) {
    UV_CALL(uv_timer_stop(&timer), loop);
  }

  UV_CALL(uv_poll_stop(&eventHandle), loop);
}

void UvEventPort::run() {
  KJ_LOG(ERROR, "run", scheduled, runnable, kjLoop.isRunnable());
  KJ_ASSERT(scheduled);

  UV_CALL(uv_timer_stop(&timer), loop);

  if (runnable) {
    kjLoop.run();
  }

  if (runnable) {
    // Apparently either we never became non-runnable, or we did but then became runnable again.
    // Since `scheduled` has been true the whole time, we won't have been rescheduled, so do that
    // now.
    KJ_LOG(WARNING, "still runnable after kjLoop.run()?");
    schedule();
  } else {
    scheduled = false;
  }
}

static constexpr uint NEW_FD_FLAGS =
#if __linux__
    kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC | kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK |
#endif
    kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP;
// We always try to open FDs with CLOEXEC and NONBLOCK already set on Linux, but on other platforms
// this is not possible.

class OwnedFileDescriptor {
public:
  OwnedFileDescriptor(uv_loop_t* loop, int fd, uint flags)
      : uvLoop(loop), fd(applyFlags(fd, flags)), flags(flags),
        uvPoller(uv_poll_init, uvLoop, fd) {
    uvPoller->data = this;
    UV_CALL(uv_poll_start(uvPoller, 0, &pollCallback), uvLoop);
  }

  ~OwnedFileDescriptor() noexcept(false) {
    if (!stopped) {
      UV_CALL(uv_poll_stop(uvPoller), uvLoop);
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
    if (stopped) return kj::READY_NOW;

    KJ_REQUIRE(readable == nullptr, "Must wait for previous event to complete.");

    auto paf = kj::newPromiseAndFulfiller<void>();
    readable = kj::mv(paf.fulfiller);

    int flags = UV_READABLE | (writable == nullptr ? 0 : UV_WRITABLE);
    UV_CALL(uv_poll_start(uvPoller, flags, &pollCallback), uvLoop);

    return kj::mv(paf.promise);
  }

  kj::Promise<void> onWritable() {
    if (stopped) return kj::READY_NOW;

    KJ_REQUIRE(writable == nullptr, "Must wait for previous event to complete.");

    auto paf = kj::newPromiseAndFulfiller<void>();
    writable = kj::mv(paf.fulfiller);

    int flags = UV_WRITABLE | (readable == nullptr ? 0 : UV_READABLE);
    UV_CALL(uv_poll_start(uvPoller, flags, &pollCallback), uvLoop);

    return kj::mv(paf.promise);
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

  static void pollCallback(uv_poll_t* handle, int status, int events) {
    reinterpret_cast<OwnedFileDescriptor*>(handle->data)->pollDone(status, events);
  }

  void pollDone(int status, int events) {
    if (status != 0) {
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

    } else {
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
      UV_CALL(uv_poll_start(uvPoller, flags, &pollCallback), uvLoop);
    }
  }
};

class UvIoStream: public OwnedFileDescriptor, public kj::AsyncIoStream {
  // IoStream implementation on top of libuv.  This is mostly a copy of the UnixEventPort-based
  // implementation in kj/async-io.c++.  We use uv_poll, which the libuv docs say is slow
  // "especially on Windows".  I'm guessing it's not so slow on Unix, since it matches the
  // underlying APIs.
  //
  // TODO(cleanup):  Allow better code sharing between the two.

public:
  UvIoStream(uv_loop_t* loop, int fd, uint flags)
      : OwnedFileDescriptor(loop, fd, flags) {}
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
    } else {
      return writeInternal(pieces[0], pieces.slice(1, pieces.size()));
    }
  }

  void shutdownWrite() override {
    // There's no legitimate way to get an AsyncStreamFd that isn't a socket through the
    // UnixAsyncIoProvider interface.
    KJ_SYSCALL(shutdown(fd, SHUT_WR));
  }

#if CAPNP_VERSION >= 8000
  kj::Promise<void> whenWriteDisconnected() override {
    // TODO(someday): Implement using UV_DISCONNECT?
    return kj::NEVER_DONE;
  }
#endif

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
    } else if (n == 0) {
      // EOF -OR- maxBytes == 0.
      return alreadyRead;
    } else if (kj::implicitCast<size_t>(n) < minBytes) {
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
    } else {
      // We read enough to stop here.
      return alreadyRead + n;
    }
  }

  kj::Promise<void> writeInternal(kj::ArrayPtr<const byte> firstPiece,
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

class UvConnectionReceiver final: public kj::ConnectionReceiver, public OwnedFileDescriptor {
  // Like UvIoStream but for ConnectionReceiver.  This is also largely copied from kj/async-io.c++.

public:
  UvConnectionReceiver(uv_loop_t* loop, int fd, uint flags)
      : OwnedFileDescriptor(loop, fd, flags) {}

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
    } else {
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
          // According to the Linux man page, accept() may report an error if the accepted
          // connection is already broken.  In this case, we really ought to just ignore it and
          // keep waiting.  But it's hard to say exactly what errors are such network errors and
          // which ones are permanent errors.  We've made a guess here.
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

class UvLowLevelAsyncIoProvider final: public kj::LowLevelAsyncIoProvider {
public:
  UvLowLevelAsyncIoProvider(UvEventPort& eventPort): eventPort(eventPort) {}

  //  inline kj::WaitScope& getWaitScope() { return waitScope; }

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
      int fd, const struct sockaddr* addr, uint addrlen, uint flags = 0) override {
    // Unfortunately connect() doesn't fit the mold of KJ_NONBLOCKING_SYSCALL, since it indicates
    // non-blocking using EINPROGRESS.
    for (;;) {
      if (::connect(fd, addr, addrlen) < 0) {
        int error = errno;
        if (error == EINPROGRESS) {
          // Fine.
          break;
        } else if (error != EINTR) {
          KJ_FAIL_SYSCALL("connect()", error) { break; }
          return kj::Own<kj::AsyncIoStream>();
        }
      } else {
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

#if CAPNP_VERSION < 7000
  kj::Own<kj::ConnectionReceiver> wrapListenSocketFd(int fd, uint flags = 0) override {
    return kj::heap<UvConnectionReceiver>(eventPort.getUvLoop(), fd, flags);
  }
#else
  kj::Own<kj::ConnectionReceiver> wrapListenSocketFd(int fd,
      kj::LowLevelAsyncIoProvider::NetworkFilter& filter, uint flags = 0) override {
    // TODO(soon): TODO(security): Actually use `filter`. Currently no API is exposed to set a
    //   filter so it's not important yet.
    return kj::heap<UvConnectionReceiver>(eventPort.getUvLoop(), fd, flags);
  }
#endif

  kj::Timer& getTimer() override {
    // TODO(soon):  Implement this.
    KJ_FAIL_ASSERT("Timers not implemented.");
  }

private:
  UvEventPort& eventPort;
};

/*
struct UvAsyncIoProvider
  : kj::AsyncIoProvider {

  UvAsyncIoProvider(UvLowLevelAsyncIoProvider& lowLevel)
    : lowLevel_{lowLevel} {
  }

  
  OneWayPipe newOneWayPipe() override {
    int fds[2]{};
    KJ_SYSCALL(pipe2(fds, O_NONBLOCK | O_CLOEXEC));
    return kj::OneWayPipe {
      lowLevel_.wrapInputFd(fds[0], NEW_FD_FLAGS),
      lowLevel.wrapOutputFd(fds[1], NEW_FD_FLAGS)
    };
  }

  TwoWayPipe newTwoWayPipe() override {
    int fds[2]{};
    int type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
    KJ_SYSCALL(socketpair(AF_UNIX, type, 0, fds));
    return kj::TwoWayPipe { {
      lowLevel_.wrapSocketFd(fds[0], NEW_FD_FLAGS),
      lowLevel_.wrapSocketFd(fds[1], NEW_FD_FLAGS)
    } };
  }

  kj::CapabilityPipe newCapabilityPipe() override {
    int fds[2]{};
    int type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
    KJ_SYSCALL(socketpair(AF_UNIX, type, 0, fds));
    return kj::CapabilityPipe { {
      lowLevel_.wrapUnixSocketFd(fds[0], NEW_FD_FLAGS),
      lowLevel_.wrapUnixSocketFd(fds[1], NEW_FD_FLAGS)
    } };
  }

  Network& getNetwork() override {
    return network;
  }

  kj::PipeThread newPipeThread(
      kj::Function<void(kj::AsyncIoProvider&, kj::AsyncIoStream&, kj::WaitScope&)> startFunc) override {
    int fds[2]{};
    int type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;

    KJ_SYSCALL(socketpair(AF_UNIX, type, 0, fds));

    int threadFd = fds[1];
    KJ_ON_SCOPE_FAILURE(close(threadFd));

    auto pipe = lowLevel_.wrapSocketFd(fds[0], NEW_FD_FLAGS);

    auto thread = kj::heap<kj::Thread>([threadFd, startFunc=kj::mv(startFunc)]() mutable {
      UvEventPort eventPort;
      kj::EventLoop eventLoop(eventPort);
      kj::WaitScope waitScope(eventLoop);
      UvLowLevelAsyncIoProvider lowLevel_{eventPort};
      auto stream = lowLevel_.wrapSocketFd(threadFd, NEW_FD_FLAGS);
      UvAsyncIoProvider ioProvider{lowLevel};
      startFunc(ioProvider, *stream, waitScope);
    });

    return { kj::mv(thread), kj::mv(pipe) };
  }

  kj::Timer& getTimer() override { return lowLevel_.getTimer(); }

private:
  UvLowLevelAsyncIoProvider& lowLevel_;
  SocketNetwork network;
};
*/

kj::Own<kj::LowLevelAsyncIoProvider> newUvLowLevelAsyncIoProvider(kj::UvEventPort& eventPort) {
  return kj::heap<UvLowLevelAsyncIoProvider>(eventPort);
}


/*
kj::Own<kj::AsyncIoProvider> newAsyncIoProvider(kj::LowLevelAsyncIoProvider& lowLevel) {
  return kj::heap<UvAsyncIoProvider>(kj::downcast<UvLowLevelAsyncIoProvider>(lowLevel));
}
*/

}

