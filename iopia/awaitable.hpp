// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <liburing.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/argument.hpp"
#include "iopia/io_error.hpp"
#include "iopia/loop.hpp"
#include "iopia/operation.hpp"
#include "iopia/ring.hpp"

// The io_uring operations a Task can co_await, NoOp, Read, Write and Timeout.
namespace iopia {
namespace internal {

// The operation behind every awaitable. It prepares its submission when the
// coroutine suspends, and resumes the coroutine with the completion's result.
// It lives in the awaiting coroutine's frame. If the loop can't hand out an
// entry, the coroutine doesn't suspend, and the result is -EBUSY.
template <typename AwaitableType>
class Awaitable : public Operation {
 public:
  DECLARE_COPY_DELETE(Awaitable);
  DECLARE_MOVE_DELETE(Awaitable);

  auto await_ready() const noexcept -> bool { return false; }

  auto await_suspend(std::coroutine_handle<> awaiting) -> bool {
    ::io_uring_sqe* submission = loop_->try_take_submission(Depend(*this));
    if (!submission) [[unlikely]] {
      result_ = -EBUSY;
      return false;
    }
    static_cast<AwaitableType&>(*this).prepare(submission);
    awaiting_ = awaiting;
    return true;
  }

 protected:
  explicit Awaitable(Depend<Loop> loop) : loop_{loop.get()} {
    complete = [](Operation& operation, Completion completion) noexcept {
      auto& self = static_cast<Awaitable&>(operation);
      self.result_ = completion.result;
      self.awaiting_.resume();
    };
  }

  ~Awaitable() = default;

  std::int32_t result_ = 0;

 private:
  Loop* loop_ = nullptr;
  std::coroutine_handle<> awaiting_;
};

}  // namespace internal

// Completes without doing anything.
class NoOp final : public internal::Awaitable<NoOp> {
 public:
  explicit NoOp(Depend<Loop> loop) : Awaitable{loop} {}

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_nop(submission);
  }

  auto await_resume() const -> std::expected<void, lib::Status> {
    if (result_ < 0) [[unlikely]] {
      return Failure{raise_io_error(result_, "no-op")};
    }
    return {};
  }
};

// Reads from `file` at `offset` into `buffer`, and returns how many bytes it
// read. An offset of -1 reads from the file's current position.
class Read final : public internal::Awaitable<Read> {
 public:
  Read(Depend<Loop> loop, std::int32_t file, std::span<std::byte> buffer,
       std::uint64_t offset)
      : Awaitable{loop}, file_{file}, buffer_{buffer}, offset_{offset} {}

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_read(submission, file_, buffer_.data(),
                         static_cast<std::uint32_t>(buffer_.size()), offset_);
  }

  auto await_resume() const -> std::expected<std::uint32_t, lib::Status> {
    if (result_ < 0) [[unlikely]] {
      return Failure{raise_io_error(result_, "read")};
    }
    return static_cast<std::uint32_t>(result_);
  }

 private:
  std::int32_t file_ = -1;
  std::span<std::byte> buffer_;
  std::uint64_t offset_ = 0;
};

// Writes `buffer` to `file` at `offset`, and returns how many bytes it wrote.
// An offset of -1 writes at the file's current position.
class Write final : public internal::Awaitable<Write> {
 public:
  Write(Depend<Loop> loop, std::int32_t file, std::span<std::byte const> buffer,
        std::uint64_t offset)
      : Awaitable{loop}, file_{file}, buffer_{buffer}, offset_{offset} {}

  auto prepare(::io_uring_sqe* submission) noexcept -> void {
    ::io_uring_prep_write(submission, file_, buffer_.data(),
                          static_cast<std::uint32_t>(buffer_.size()), offset_);
  }

  auto await_resume() const -> std::expected<std::uint32_t, lib::Status> {
    if (result_ < 0) [[unlikely]] {
      return Failure{raise_io_error(result_, "write")};
    }
    return static_cast<std::uint32_t>(result_);
  }

 private:
  std::int32_t file_ = -1;
  std::span<std::byte const> buffer_;
  std::uint64_t offset_ = 0;
};

// Completes once `duration` has passed.
class Timeout final : public internal::Awaitable<Timeout> {
 public:
  Timeout(Depend<Loop> loop, std::chrono::nanoseconds duration)
      : Awaitable{loop},
        duration_{
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
  auto await_resume() const -> std::expected<void, lib::Status> {
    if (result_ < 0 && result_ != -ETIME) [[unlikely]] {
      return Failure{raise_io_error(result_, "timeout")};
    }
    return {};
  }

 private:
  ::__kernel_timespec duration_;
};

}  // namespace iopia
