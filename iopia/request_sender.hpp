// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cerrno>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "base/core.hpp"
#include "core/argument.hpp"
#include "iopia/io_error.hpp"
#include "iopia/loop.hpp"
#include "iopia/operation.hpp"
#include "iopia/request.hpp"
#include "iopia/ring.hpp"
#include "iopia/sender.hpp"

// The io_uring requests as senders, NoOpSender, ReadSender, WriteSender and
// TimeoutSender, and LoopScheduler.
namespace iopia {
namespace internal {

// The operation state of a request sender. start() takes an entry and prepares
// the request; the completion turns the result into set_value or set_error, or
// set_stopped when the request was cancelled. If the loop can't hand out an
// entry, start() completes at once with set_error and IoError::BUSY.
template <typename RequestType, typename ReceiverType>
class RequestOperation final : public Operation {
 public:
  DECLARE_COPY_DELETE(RequestOperation);
  DECLARE_MOVE_DELETE(RequestOperation);

  RequestOperation(Depend<Loop> loop, RequestType request,
                   ReceiverType receiver)
      : loop_{loop.get()},
        request_{std::move(request)},
        receiver_{std::move(receiver)} {
    complete = [](Operation& operation, Completion completion) noexcept {
      static_cast<RequestOperation&>(operation).finish(completion.result);
    };
  }

  ~RequestOperation() = default;

  auto start() noexcept -> void {
    ::io_uring_sqe* submission = loop_->try_take_submission(Depend(*this));
    if (!submission) [[unlikely]] {
      std::move(receiver_).set_error(raise_queue_full());
      return;
    }
    request_.prepare(submission);
  }

 private:
  auto finish(std::int32_t result) noexcept -> void {
    if (result == -ECANCELED) {
      std::move(receiver_).set_stopped();
      return;
    }
    auto value = request_.result(result);
    if (!value) {
      std::move(receiver_).set_error(std::move(value).error());
    } else if constexpr (std::is_void_v<typename RequestType::value_type>) {
      std::move(receiver_).set_value();
    } else {
      std::move(receiver_).set_value(*std::move(value));
    }
  }

  // Fields go by lifetime, meaning, cache heat, then descending size.
  Loop* loop_ = nullptr;
  RequestType request_;
  ReceiverType receiver_;
};

}  // namespace internal

// A sender of one io_uring request.
template <typename RequestType>
class RequestSender final {
 public:
  using value_type = typename RequestType::value_type;

  // Makes the request from `arguments`.
  template <typename... ArgumentTypes>
  explicit RequestSender(Depend<Loop> loop, ArgumentTypes&&... arguments)
      : request_(std::forward<ArgumentTypes>(arguments)...),
        loop_{loop.get()} {}

  template <Receiver<value_type> ReceiverType>
  auto connect(ReceiverType receiver) && {
    return internal::RequestOperation<RequestType, ReceiverType>{
        Depend(*loop_), std::move(request_), std::move(receiver)};
  }

  template <Receiver<value_type> ReceiverType>
  auto connect(ReceiverType receiver) const& {
    return internal::RequestOperation<RequestType, ReceiverType>{
        Depend(*loop_), request_, std::move(receiver)};
  }

 private:
  RequestType request_;
  Loop* loop_ = nullptr;
};

// Completes without doing anything: NoOpSender{loop}.
using NoOpSender = RequestSender<NoOpRequest>;

// Reads into a buffer, and completes with how many bytes it read:
// ReadSender{loop, file, buffer, offset}.
using ReadSender = RequestSender<ReadRequest>;

// Writes a buffer, and completes with how many bytes it wrote:
// WriteSender{loop, file, buffer, offset}.
using WriteSender = RequestSender<WriteRequest>;

// Completes once a duration has passed: TimeoutSender{loop, duration}.
using TimeoutSender = RequestSender<TimeoutRequest>;

// Work scheduled here completes from the loop's dispatch, on the loop's thread.
class LoopScheduler final {
 public:
  explicit LoopScheduler(Depend<Loop> loop) : loop_{loop.get()} {}

  auto schedule() const -> NoOpSender { return NoOpSender{Depend(*loop_)}; }

  friend auto operator==(LoopScheduler, LoopScheduler) -> bool = default;

 private:
  Loop* loop_ = nullptr;
};

}  // namespace iopia
