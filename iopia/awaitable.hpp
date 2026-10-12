// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cerrno>
#include <coroutine>
#include <cstdint>
#include <utility>

#include "base/core.hpp"
#include "iopia/argument.hpp"
#include "iopia/loop.hpp"
#include "iopia/operation.hpp"
#include "iopia/request.hpp"
#include "iopia/ring.hpp"

// The io_uring operations a Task can co_await, NoOp, Read, Write and Timeout.
namespace iopia {
namespace internal {

// The operation behind every awaitable. It prepares its request's submission
// when the coroutine suspends, and resumes the coroutine with the request's
// result. It lives in the awaiting coroutine's frame. If the loop can't hand
// out an entry, the coroutine doesn't suspend, and the result is -EBUSY.
template <typename RequestType>
class Awaitable final : public Operation {
 public:
  DECLARE_COPY_DELETE(Awaitable);
  DECLARE_MOVE_DELETE(Awaitable);

  // Makes the request from `arguments`.
  template <typename... ArgumentTypes>
  explicit Awaitable(Depend<Loop> loop, ArgumentTypes&&... arguments)
      : request_(std::forward<ArgumentTypes>(arguments)...), loop_{loop.get()} {
    complete = [](Operation& operation, Completion completion) noexcept {
      auto& self = static_cast<Awaitable&>(operation);
      self.result_ = completion.result;
      self.awaiting_.resume();
    };
  }

  ~Awaitable() = default;

  auto await_ready() const noexcept -> bool { return false; }

  auto await_suspend(std::coroutine_handle<> awaiting) -> bool {
    ::io_uring_sqe* submission = loop_->try_take_submission(Depend(*this));
    if (!submission) [[unlikely]] {
      result_ = -EBUSY;
      return false;
    }
    request_.prepare(submission);
    awaiting_ = awaiting;
    return true;
  }

  auto await_resume() const { return request_.result(result_); }

 private:
  RequestType request_;
  std::int32_t result_ = 0;
  Loop* loop_ = nullptr;
  std::coroutine_handle<> awaiting_;
};

}  // namespace internal

// Completes without doing anything: NoOp{loop}.
using NoOp = internal::Awaitable<NoOpRequest>;

// Reads into a buffer, and returns how many bytes it read:
// Read{loop, file, buffer, offset}.
using Read = internal::Awaitable<ReadRequest>;

// Writes a buffer, and returns how many bytes it wrote:
// Write{loop, file, buffer, offset}.
using Write = internal::Awaitable<WriteRequest>;

// Completes once a duration has passed: Timeout{loop, duration}.
using Timeout = internal::Awaitable<TimeoutRequest>;

}  // namespace iopia
