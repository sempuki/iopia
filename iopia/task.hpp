// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <coroutine>
#include <exception>
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/argument.hpp"
#include "iopia/io_error.hpp"
#include "iopia/loop.hpp"

// Task<T>, a lazy coroutine, and run(), which drives one to completion on a
// loop.
namespace iopia {

template <typename ValueType>
class Task;

namespace internal {

// What every task's promise shares: it starts suspended, and when it finishes
// it resumes the coroutine that awaited it, or returns to whoever resumed it.
class PromiseBase {
 public:
  // A task that finishes while its awaiter is still starting it returns to
  // the awaiter's await_suspend. One that finishes later resumes the awaiter.
  struct FinalAwaiter final {
    auto await_ready() const noexcept -> bool { return false; }

    template <typename PromiseType>
    auto await_suspend(std::coroutine_handle<PromiseType> finished) noexcept
        -> std::coroutine_handle<> {
      PromiseBase& promise = finished.promise();
      if (promise.starting_) {
        return std::noop_coroutine();
      }
      return promise.continuation_;
    }

    auto await_resume() const noexcept -> void {}
  };

  auto initial_suspend() const noexcept -> std::suspend_always { return {}; }
  auto final_suspend() const noexcept -> FinalAwaiter { return {}; }

  // iopia doesn't use exceptions.
  [[noreturn]] auto unhandled_exception() const noexcept -> void {
    std::terminate();
  }

  std::coroutine_handle<> continuation_ = std::noop_coroutine();
  bool starting_ = false;
};

template <typename ValueType>
class Promise final : public PromiseBase {
 public:
  auto get_return_object() noexcept -> Task<ValueType>;

  template <typename ResultType>
    requires std::is_convertible_v<ResultType&&, ValueType>
  auto return_value(ResultType&& result) -> void {
    value_.emplace(std::forward<ResultType>(result));
  }

  auto take_value() -> ValueType {
    CHECK_PRECONDITION(value_);
    return *std::move(value_);
  }

 private:
  std::optional<ValueType> value_;
};

template <>
class Promise<void> final : public PromiseBase {
 public:
  auto get_return_object() noexcept -> Task<void>;
  auto return_void() const noexcept -> void {}
  auto take_value() const noexcept -> void {}
};

}  // namespace internal

// A coroutine that returns a ValueType. It is lazy: it starts when it is
// awaited, or when run() is given it, and it is move-only. Awaiting it from
// another task resumes that task when it finishes.
template <typename ValueType>
class [[nodiscard]] Task final {
 public:
  using promise_type = internal::Promise<ValueType>;

  DECLARE_COPY_DELETE(Task);

  Task(Task&& that) noexcept
      : coroutine_{std::exchange(that.coroutine_, nullptr)} {}

  auto operator=(Task&& that) noexcept -> Task& {
    if (this != &that) {
      destroy();
      coroutine_ = std::exchange(that.coroutine_, nullptr);
    }
    return *this;
  }

  ~Task() { destroy(); }

  // Starts the task from a coroutine that awaits it, and resumes that
  // coroutine with the task's value when it finishes. If the task finishes
  // before it suspends, the awaiting coroutine continues without suspending.
  auto operator co_await() && noexcept {
    struct Awaiter final {
      auto await_ready() const noexcept -> bool { return false; }

      auto await_suspend(std::coroutine_handle<> awaiting) noexcept -> bool {
        internal::PromiseBase& promise = coroutine.promise();
        promise.continuation_ = awaiting;
        promise.starting_ = true;
        coroutine.resume();
        promise.starting_ = false;
        return !coroutine.done();
      }

      auto await_resume() -> ValueType {
        return coroutine.promise().take_value();
      }

      std::coroutine_handle<promise_type> coroutine;
    };
    CHECK_PRECONDITION(coroutine_);
    return Awaiter{coroutine_};
  }

 private:
  friend promise_type;

  template <typename RunValueType>
  friend auto run(InOut<Loop> loop, Task<RunValueType> task)
      -> std::expected<RunValueType, lib::Status>;

  explicit Task(std::coroutine_handle<promise_type> coroutine) noexcept
      : coroutine_{coroutine} {}

  auto destroy() noexcept -> void {
    if (coroutine_) {
      coroutine_.destroy();
    }
  }

  std::coroutine_handle<promise_type> coroutine_;
};

template <typename ValueType>
auto internal::Promise<ValueType>::get_return_object() noexcept
    -> Task<ValueType> {
  return Task<ValueType>{
      std::coroutine_handle<Promise<ValueType>>::from_promise(*this)};
}

inline auto internal::Promise<void>::get_return_object() noexcept
    -> Task<void> {
  return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

// Starts `task` and runs `loop` until the task finishes, then returns its
// value. An interrupted wait is retried. Any other error from the loop is
// returned at once.
template <typename ValueType>
auto run(InOut<Loop> loop, Task<ValueType> task)
    -> std::expected<ValueType, lib::Status> {
  CHECK_PRECONDITION(task.coroutine_);
  task.coroutine_.resume();
  while (!task.coroutine_.done()) {
    // A task that waits on nothing the loop knows about would never finish.
    CHECK_INVARIANT(loop->pending() > 0);
    auto dispatched = loop->run_once();
    if (!dispatched && dispatched.error() != lib::watch(IoError::INTERRUPTED)) {
      return Failure{std::move(dispatched).error()};
    }
  }
  if constexpr (std::is_void_v<ValueType>) {
    return {};
  } else {
    return task.coroutine_.promise().take_value();
  }
}

}  // namespace iopia
