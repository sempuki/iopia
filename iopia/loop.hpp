// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/operation.hpp"
#include "iopia/ring.hpp"

// The event loop: it submits operations and calls each one back when its
// completions arrive.
namespace iopia {

// A function that prepares a submission queue entry, which it takes as an
// ::io_uring_sqe*, and can't throw.
template <typename PrepareType>
concept Preparer = std::is_nothrow_invocable_v<PrepareType, ::io_uring_sqe*>;

namespace internal {

// The node that holds a handler the loop owns. It goes back to its free list
// before its handler's last call.
struct HandlerNode final : Operation {
  explicit HandlerNode(lib::Depend<std::vector<HandlerNode*>> free);

  std::move_only_function<void(Completion) noexcept> handler;
  std::vector<HandlerNode*>* free_ = nullptr;
};

// The operation of the loop's own cancel request, which needs no handler.
struct IgnoredOperation final : Operation {
  IgnoredOperation() {
    complete = [](Operation&, Completion) noexcept {};
  }
};

}  // namespace internal

// Owns a Ring, ties each submission to the Operation it completes, and counts
// the operations whose last completion hasn't arrived. It reaps completions in
// batches, and runs until nothing is pending or a handler calls stop(). Every
// submission's user_data is the address of its Operation, so a completion whose
// user_data is 0 has no operation and fails a contract check. A Loop can be
// neither copied nor moved, because its nodes and its operations hold its
// address. Destroying it cancels what is pending and waits for every last
// completion.
class Loop final {
 public:
  DECLARE_COPY_DELETE(Loop);
  DECLARE_MOVE_DELETE(Loop);

  explicit Loop(Ring ring) : ring_{std::move(ring)} {}
  ~Loop();

  // The next free submission queue entry, with its user_data set to
  // `operation`. The caller prepares it with an io_uring_prep_* function. If
  // the queue is full, it submits what the queue holds and tries once more,
  // and returns null if that fails or the queue is still full.
  auto maybe_submission(lib::Depend<Operation> operation) -> ::io_uring_sqe* {
    CHECK_PRECONDITION(operation->complete);
    ::io_uring_sqe* submission = ring_.maybe_submission();
    if (!submission && ring_.submit()) [[unlikely]] {
      submission = ring_.maybe_submission();
    }
    if (submission) {
      ::io_uring_sqe_set_data(submission, operation.get());
      ++pending_;
    }
    return submission;
  }

  // Prepares a submission with `prepare`, which takes the ::io_uring_sqe*, and
  // has the loop keep `handler` and call it with each completion. Returns
  // false, and keeps nothing, if the queue is full.
  template <typename PrepareType, typename HandlerType>
    requires Preparer<PrepareType> && Handler<std::decay_t<HandlerType>>
  [[nodiscard]] auto queue(PrepareType&& prepare, HandlerType&& handler)
      -> bool {
    internal::HandlerNode& node = acquire_node();
    ::io_uring_sqe* submission = maybe_submission(lib::Depend(node));
    if (!submission) {
      free_.push_back(&node);
      return false;
    }
    node.handler = std::forward<HandlerType>(handler);
    std::invoke(std::forward<PrepareType>(prepare), submission);
    return true;
  }

  // Hands every prepared entry to the kernel, and returns how many it took.
  auto submit() -> std::expected<unsigned, lib::Status> {
    return ring_.submit();
  }

  // Submits what is prepared, waits for at least one completion, and calls
  // the operation of every completion that is ready. Returns how many it
  // called, or 0 at once if nothing is pending.
  auto run_once() -> std::expected<unsigned, lib::Status> {
    if (pending_ == 0) {
      return 0u;
    }
    RETURN_IF_UNEXPECTED(ring_.submit_and_wait(1));
    return dispatch_ready();
  }

  // Calls run_once() until nothing is pending or a handler calls stop(). An
  // interrupted wait is retried; any other error is returned at once.
  auto run() -> std::expected<void, lib::Status>;

  // Makes run() return once the completions it is dispatching are done. It
  // affects only the run() in progress.
  auto stop() -> void { stopping_ = true; }

  // How many operations have a submission whose last completion hasn't been
  // dispatched.
  auto pending() const -> std::size_t { return pending_; }

 private:
  auto dispatch(Completion completion) noexcept -> void {
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

  // Copies the ready completions a batch at a time, returns each batch to the
  // kernel, then dispatches it. It stops after a batch that isn't full, so
  // completions that keep arriving can't hold it forever.
  auto dispatch_ready() -> unsigned {
    std::array<Completion, COMPLETION_BATCH> batch;
    unsigned dispatched = 0;
    std::size_t taken = 0;
    do {
      taken = ring_.take_completions(batch);
      for (Completion completion : std::span{batch}.first(taken)) {
        dispatch(completion);
      }
      dispatched += static_cast<unsigned>(taken);
    } while (taken == batch.size());
    return dispatched;
  }

  // A node from the free list, or a new one if the list is empty.
  auto acquire_node() -> internal::HandlerNode&;

  // Cancels every pending operation and runs until none is pending.
  auto cancel_and_drain() -> void;

  // How many completions dispatch_ready() copies out of the ring at once.
  static constexpr std::size_t COMPLETION_BATCH = 32;

  Ring ring_;
  std::size_t pending_ = 0;
  bool stopping_ = false;
  std::vector<std::unique_ptr<internal::HandlerNode>> nodes_;
  std::vector<internal::HandlerNode*> free_;
  internal::IgnoredOperation cancel_;
};

}  // namespace iopia
