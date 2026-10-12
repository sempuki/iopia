// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "base/status.hpp"
#include "core/status.hpp"
#include "iopia/io_error.hpp"

// The io_uring requests both front-ends issue. A request prepares its
// submission queue entry and turns its completion's result into a value. The
// awaitables and the senders build on the same requests.
namespace iopia {

// Completes without doing anything.
struct NoOpRequest final {
  using value_type = void;

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_nop(submission);
  }

  auto result(std::int32_t result) const -> std::expected<void, Status> {
    if (result < 0) [[unlikely]] {
      return Failure{raise_io_error(result, "no-op")};
    }
    return {};
  }
};

// Reads from `file` at `offset` into `buffer`, and returns how many bytes it
// read. An offset of -1 reads from the file's current position.
struct ReadRequest final {
  using value_type = std::uint32_t;

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_read(submission, file, buffer.data(),
                         static_cast<std::uint32_t>(buffer.size()), offset);
  }

  auto result(std::int32_t result) const
      -> std::expected<std::uint32_t, Status> {
    if (result < 0) [[unlikely]] {
      return Failure{raise_io_error(result, "read")};
    }
    return static_cast<std::uint32_t>(result);
  }

  std::int32_t file = -1;
  std::span<std::byte> buffer;
  std::uint64_t offset = 0;
};

// Writes `buffer` to `file` at `offset`, and returns how many bytes it wrote.
// An offset of -1 writes at the file's current position.
struct WriteRequest final {
  using value_type = std::uint32_t;

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_write(submission, file, buffer.data(),
                          static_cast<std::uint32_t>(buffer.size()), offset);
  }

  auto result(std::int32_t result) const
      -> std::expected<std::uint32_t, Status> {
    if (result < 0) [[unlikely]] {
      return Failure{raise_io_error(result, "write")};
    }
    return static_cast<std::uint32_t>(result);
  }

  std::int32_t file = -1;
  std::span<std::byte const> buffer;
  std::uint64_t offset = 0;
};

// Completes once `duration` has passed.
class TimeoutRequest final {
 public:
  using value_type = void;

  explicit TimeoutRequest(std::chrono::nanoseconds duration)
      : duration_{
            .tv_sec =
                std::chrono::floor<std::chrono::seconds>(duration).count(),
            .tv_nsec = (duration % std::chrono::seconds{1}).count(),
        } {}

  // The kernel reads `duration_` when the entry is submitted, after prepare
  // returns.
  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_timeout(submission, &duration_, 0, 0);
  }

  // A timeout that expires completes with -ETIME, which is its success.
  auto result(std::int32_t result) const -> std::expected<void, Status> {
    if (result < 0 && result != -ETIME) [[unlikely]] {
      return Failure{raise_io_error(result, "timeout")};
    }
    return {};
  }

 private:
  ::__kernel_timespec duration_;
};

}  // namespace iopia
