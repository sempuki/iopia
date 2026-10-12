// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <coroutine>
#include <cstdint>
#include <utility>

#include "base/core.hpp"
#include "core/argument.hpp"
#include "iopia/io_error.hpp"
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
// out an entry, the coroutine doesn't suspend, and the result is
// IoError::BUSY.
template <typename RequestType>
class AwaitableOperation final : public Operation {
 public:
  DECLARE_COPY_DELETE(AwaitableOperation);
  DECLARE_MOVE_DELETE(AwaitableOperation);

  // Makes the request from `arguments`.
  template <typename... ArgumentTypes>
  explicit AwaitableOperation(Depend<Loop> loop, ArgumentTypes&&... arguments)
      : loop_{loop.get()}, request_(std::forward<ArgumentTypes>(arguments)...) {
    complete = [](Operation& operation, Completion completion) noexcept {
      auto& self = static_cast<AwaitableOperation&>(operation);
      self.result_ = completion.result;
      self.awaiting_.resume();
    };
  }

  ~AwaitableOperation() = default;

  auto await_ready() const noexcept -> bool { return false; }

  auto await_suspend(std::coroutine_handle<> awaiting) -> bool {
    ::io_uring_sqe* submission = loop_->try_take_submission(Depend(*this));
    if (!submission) [[unlikely]] {
      return false;
    }
    request_.prepare(submission);
    awaiting_ = awaiting;
    submitted_ = true;
    return true;
  }

  auto await_resume() const
      -> decltype(std::declval<RequestType const&>().result(0)) {
    if (!submitted_) [[unlikely]] {
      return Failure{raise_queue_full()};
    }
    return request_.result(result_);
  }

 private:
  // Fields go by lifetime, meaning, cache heat, then descending size.
  Loop* loop_ = nullptr;
  RequestType request_;
  std::coroutine_handle<> awaiting_;
  std::int32_t result_ = 0;
  bool submitted_ = false;
};

}  // namespace internal

// Completes without doing anything: NoOp{loop}.
using NoOp = internal::AwaitableOperation<NoOpRequest>;

// Reads into a buffer, and returns how many bytes it read:
// Read{loop, file, buffer, offset}.
using Read = internal::AwaitableOperation<ReadRequest>;

// Writes a buffer, and returns how many bytes it wrote:
// Write{loop, file, buffer, offset}.
using Write = internal::AwaitableOperation<WriteRequest>;

// Completes once a duration has passed: Timeout{loop, duration}.
using Timeout = internal::AwaitableOperation<TimeoutRequest>;

}  // namespace iopia
