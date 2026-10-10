// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/operation.hpp"
#include "iopia/ring.hpp"

// The event loop: it submits operations and calls each one back when its
// completions arrive.
namespace iopia {

namespace internal {

// The node that holds a callable the loop owns. It goes back to its free list
// before its last callback runs.
struct CallbackOperation final : Operation {
  explicit CallbackOperation(lib::Depend<std::vector<CallbackOperation*>> free);

  std::move_only_function<void(Completion)> callback;
  std::vector<CallbackOperation*>* free_ = nullptr;
};

// The operation of the loop's own cancel request, which needs no callback.
struct IgnoredOperation final : Operation {
  IgnoredOperation() {
    complete = [](Operation&, Completion) {};
  }
};

}  // namespace internal

// Owns a Ring, ties each submission to the Operation it completes, and counts
// the operations whose last completion hasn't arrived. Every submission's
// user_data is the address of its Operation, so a completion whose user_data is
// 0 has no operation and fails a contract check. A Loop can be neither copied
// nor moved, because its nodes and its operations hold its address. Destroying
// it cancels what is pending and waits for every last completion.
class Loop final {
 public:
  DECLARE_COPY_DELETE(Loop);
  DECLARE_MOVE_DELETE(Loop);

  explicit Loop(Ring ring) : ring_{std::move(ring)} {}
  ~Loop();

  // The next free submission queue entry, with its user_data set to
  // `operation`, or null if the queue is full. The caller prepares it with an
  // io_uring_prep_* function.
  auto maybe_submission(lib::Depend<Operation> operation) -> ::io_uring_sqe* {
    CHECK_PRECONDITION(operation->complete);
    ::io_uring_sqe* submission = ring_.maybe_submission();
    if (submission) {
      ::io_uring_sqe_set_data(submission, operation.get());
      ++pending_;
    }
    return submission;
  }

  // Prepares a submission with `prepare`, which takes the ::io_uring_sqe*, and
  // has the loop keep `callback` and call it with each completion. Returns
  // false, and keeps nothing, if the queue is full.
  template <typename PrepareType, typename CallbackType>
  [[nodiscard]] auto queue(PrepareType&& prepare, CallbackType&& callback)
      -> bool {
    internal::CallbackOperation& node = acquire_node();
    ::io_uring_sqe* submission = maybe_submission(lib::Depend(node));
    if (!submission) {
      free_.push_back(&node);
      return false;
    }
    node.callback = std::forward<CallbackType>(callback);
    std::invoke(std::forward<PrepareType>(prepare), submission);
    return true;
  }

  // Hands every prepared entry to the kernel, and returns how many it took.
  auto submit() -> std::expected<unsigned, lib::Status> {
    return ring_.submit();
  }

  // Submits what is prepared, waits for at least one completion, and calls
  // back the operation of every completion that is ready. Returns how many it
  // called back, or 0 at once if nothing is pending.
  auto run_once() -> std::expected<unsigned, lib::Status> {
    if (pending_ == 0) {
      return 0u;
    }
    RETURN_IF_UNEXPECTED(ring_.submit_and_wait(1));
    unsigned dispatched = 0;
    while (auto completion = ring_.maybe_completion()) {
      ++dispatched;
      dispatch(*completion);
    }
    return dispatched;
  }

  // How many operations have a submission whose last completion hasn't been
  // dispatched.
  auto pending() const -> std::size_t { return pending_; }

 private:
  auto dispatch(Completion completion) -> void {
    auto* operation = reinterpret_cast<Operation*>(
        static_cast<std::uintptr_t>(completion.user_data));
    CHECK_INVARIANT(operation);
    CHECK_INVARIANT(operation->complete);
    if (!(completion.flags & IORING_CQE_F_MORE)) {
      CHECK_INVARIANT(pending_ > 0);
      --pending_;
    }
    operation->complete(*operation, completion);
  }

  // A node from the free list, or a new one if the list is empty.
  auto acquire_node() -> internal::CallbackOperation&;

  // Cancels every pending operation and runs until none is pending.
  auto cancel_and_drain() -> void;

  Ring ring_;
  std::size_t pending_ = 0;
  std::vector<std::unique_ptr<internal::CallbackOperation>> nodes_;
  std::vector<internal::CallbackOperation*> free_;
  internal::IgnoredOperation cancel_;
};

}  // namespace iopia
