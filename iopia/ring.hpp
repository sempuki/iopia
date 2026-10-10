// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cstdint>
#include <expected>
#include <memory>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/io_error.hpp"

// One io_uring, its submission and completion queues, used from one thread.
namespace iopia {

// What the kernel reports for one finished submission. It is a copy of the
// completion queue entry, so the entry goes back to the kernel at once.
struct Completion final {
  std::uint64_t user_data = 0;  // The submission's user_data, unchanged.
  std::uint32_t flags = 0;      // IORING_CQE_F_* bits.
  std::int32_t result = 0;      // The operation's result, or a negative errno.
};

namespace internal {

// Tears down an io_uring that io_uring_queue_init set up, and frees it.
struct ExitRing final {
  auto operator()(::io_uring* ring) const noexcept -> void;
};

}  // namespace internal

// Owns one io_uring, which lives on the heap so that moves are the defaults.
// Design.md's Ring section weighs this against keeping it by value.
class Ring final {
 public:
  DECLARE_COPY_DELETE(Ring);
  DECLARE_MOVE_DEFAULT(Ring);

  ~Ring() = default;

  // A ring with at least `entries` submission queue entries. `flags` are
  // liburing's IORING_SETUP_* bits, passed through.
  static auto make(unsigned entries, unsigned flags = 0)
      -> std::expected<Ring, lib::Status>;

  // The next free submission queue entry, or null if the queue is full. The
  // caller prepares it with an io_uring_prep_* function before submit().
  auto maybe_submission() -> ::io_uring_sqe* {
    CHECK_PRECONDITION(ring_);
    return ::io_uring_get_sqe(ring_.get());
  }

  // Hands every prepared entry to the kernel, and returns how many it took.
  auto submit() -> std::expected<unsigned, lib::Status> {
    CHECK_PRECONDITION(ring_);
    int submitted = ::io_uring_submit(ring_.get());
    if (submitted < 0) [[unlikely]] {
      return Failure{raise_io_error(submitted, "io_uring_submit")};
    }
    return static_cast<unsigned>(submitted);
  }

  // Waits for the next completion and returns a copy of it.
  auto wait() -> std::expected<Completion, lib::Status> {
    CHECK_PRECONDITION(ring_);
    ::io_uring_cqe* entry = nullptr;
    if (int result = ::io_uring_wait_cqe(ring_.get(), &entry); result < 0)
        [[unlikely]] {
      return Failure{raise_io_error(result, "io_uring_wait_cqe")};
    }
    Completion completion{
        .user_data = entry->user_data,
        .flags = entry->flags,
        .result = entry->res,
    };
    ::io_uring_cqe_seen(ring_.get(), entry);
    return completion;
  }

 private:
  explicit Ring(::io_uring* ring) : ring_{ring} {}

  std::unique_ptr<::io_uring, internal::ExitRing> ring_;
};

}  // namespace iopia
