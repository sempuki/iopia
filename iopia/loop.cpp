// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/loop.hpp"

#include "iopia/io_error.hpp"

namespace iopia {

internal::HandlerNode::HandlerNode(lib::Depend<std::vector<HandlerNode*>> free)
    : free_{free.get()} {
  complete = [](Operation& operation, Completion completion) noexcept {
    auto& self = static_cast<HandlerNode&>(operation);
    if (completion.flags & IORING_CQE_F_MORE) {
      self.handler(completion);
      return;
    }
    // The node is free before the handler's last call, so the handler may
    // queue an operation that reuses it.
    auto handler = std::move(self.handler);
    self.handler = nullptr;
    self.free_->push_back(&self);
    handler(completion);
  };
}

Loop::~Loop() { cancel_and_drain(); }

auto Loop::acquire_node() -> internal::HandlerNode& {
  if (free_.empty()) {
    nodes_.push_back(
        std::make_unique<internal::HandlerNode>(lib::Depend(free_)));
    return *nodes_.back();
  }
  internal::HandlerNode* node = free_.back();
  free_.pop_back();
  return *node;
}

auto Loop::cancel_and_drain() -> void {
  if (pending_ == 0) {
    return;
  }
  ::io_uring_sqe* submission = maybe_submission(lib::Depend(cancel_));
  if (!submission) {
    // The queue is full; submitting what it holds makes room.
    auto _ = ring_.submit();
    submission = maybe_submission(lib::Depend(cancel_));
  }
  if (submission) {
    ::io_uring_prep_cancel(submission, nullptr,
                           IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
  }
  while (pending_ > 0) {
    auto dispatched = run_once();
    if (!dispatched &&
        dispatched.error().kind() != lib::watch(IoError::INTERRUPTED)) {
      // Waiting can't make progress; give up on what is left.
      return;
    }
  }
}

}  // namespace iopia
