// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <cerrno>
#include <concepts>
#include <expected>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "base/core.hpp"
#include "base/status.hpp"
#include "iopia/argument.hpp"
#include "iopia/io_error.hpp"
#include "iopia/loop.hpp"

// iopia's senders and receivers, modeled on C++26's std::execution: the
// concepts, and the algorithms just, then and sync_wait.
namespace iopia {

// The type a sender completes with. It may be void.
template <typename SenderType>
using value_of_t = typename std::remove_cvref_t<SenderType>::value_type;

namespace internal {

template <typename ReceiverType, typename ValueType>
concept ReceiverOfValue = requires(ReceiverType&& receiver, ValueType&& value) {
  { std::move(receiver).set_value(std::forward<ValueType>(value)) } noexcept;
};

template <typename ReceiverType>
concept ReceiverOfVoid = requires(ReceiverType&& receiver) {
  { std::move(receiver).set_value() } noexcept;
};

}  // namespace internal

// Takes the completion of a sender of ValueType: set_value with the value,
// set_error with a lib::Status, or set_stopped. Exactly one of them is called,
// once, on an rvalue receiver, and none can throw.
template <typename ReceiverType, typename ValueType>
concept Receiver =
    std::move_constructible<ReceiverType> &&
    requires(ReceiverType&& receiver, lib::Status error) {
      { std::move(receiver).set_error(std::move(error)) } noexcept;
      { std::move(receiver).set_stopped() } noexcept;
    } &&
    ((std::is_void_v<ValueType> && internal::ReceiverOfVoid<ReceiverType>) ||
     (!std::is_void_v<ValueType> &&
      internal::ReceiverOfValue<ReceiverType, ValueType>));

// Work in progress. start() begins it. It must stay at one address until it
// has called its receiver, so it can be neither copied nor moved.
template <typename OperationStateType>
concept OperationState = std::is_object_v<OperationStateType> &&
                         std::destructible<OperationStateType> &&
                         requires(OperationStateType& operation) {
                           { operation.start() } noexcept;
                         };

// A description of work that completes with a value_type. connect(receiver)
// turns it into an operation state that will complete into the receiver.
template <typename SenderType>
concept Sender = requires { typename value_of_t<SenderType>; };

template <typename SenderType, typename ReceiverType>
concept SenderTo =
    Sender<SenderType> && Receiver<ReceiverType, value_of_t<SenderType>> &&
    requires(SenderType&& sender, ReceiverType&& receiver) {
      {
        std::forward<SenderType>(sender).connect(
            std::forward<ReceiverType>(receiver))
      } -> OperationState;
    };

// Where work runs. schedule() returns a sender that completes there.
template <typename SchedulerType>
concept Scheduler =
    std::copy_constructible<SchedulerType> &&
    std::equality_comparable<SchedulerType> &&
    requires(SchedulerType const& scheduler) {
      { scheduler.schedule() } -> Sender;
    } &&
    std::is_void_v<
        value_of_t<decltype(std::declval<SchedulerType const&>().schedule())>>;

// just(value) completes at once with `value`.
template <typename ValueType>
class JustSender final {
 public:
  using value_type = ValueType;

  explicit JustSender(ValueType value) : value_{std::move(value)} {}

  template <Receiver<ValueType> ReceiverType>
  class State final {
   public:
    DECLARE_COPY_DELETE(State);
    DECLARE_MOVE_DELETE(State);

    State(ValueType value, ReceiverType receiver)
        : value_{std::move(value)}, receiver_{std::move(receiver)} {}

    ~State() = default;

    auto start() noexcept -> void {
      std::move(receiver_).set_value(std::move(value_));
    }

   private:
    ValueType value_;
    ReceiverType receiver_;
  };

  template <Receiver<ValueType> ReceiverType>
  auto connect(ReceiverType receiver) && -> State<ReceiverType> {
    return {std::move(value_), std::move(receiver)};
  }

 private:
  ValueType value_;
};

template <typename ValueType>
auto just(ValueType value) -> JustSender<ValueType> {
  return JustSender<ValueType>{std::move(value)};
}

namespace internal {

// What a function returns when called with a sender's value, or with nothing
// when the value is void.
template <typename FunctionType, typename ValueType>
struct ThenResult {
  using type = std::invoke_result_t<FunctionType&, ValueType>;
};

template <typename FunctionType>
struct ThenResult<FunctionType, void> {
  using type = std::invoke_result_t<FunctionType&>;
};

template <typename FunctionType, typename ValueType>
concept NothrowInvocableWith =
    (std::is_void_v<ValueType> && std::is_nothrow_invocable_v<FunctionType&>) ||
    (!std::is_void_v<ValueType> &&
     std::is_nothrow_invocable_v<FunctionType&, ValueType>);

// Calls the function with the value, and passes what it returns on. Errors
// and stops pass straight through.
template <typename FunctionType, typename ReceiverType>
class ThenReceiver final {
 public:
  ThenReceiver(FunctionType function, ReceiverType receiver)
      : function_{std::move(function)}, receiver_{std::move(receiver)} {}

  template <typename... ValueTypes>
  auto set_value(ValueTypes&&... values) && noexcept -> void {
    if constexpr (std::is_void_v<
                      std::invoke_result_t<FunctionType&, ValueTypes...>>) {
      std::invoke(function_, std::forward<ValueTypes>(values)...);
      std::move(receiver_).set_value();
    } else {
      std::move(receiver_).set_value(
          std::invoke(function_, std::forward<ValueTypes>(values)...));
    }
  }

  auto set_error(lib::Status error) && noexcept -> void {
    std::move(receiver_).set_error(std::move(error));
  }

  auto set_stopped() && noexcept -> void { std::move(receiver_).set_stopped(); }

 private:
  FunctionType function_;
  ReceiverType receiver_;
};

}  // namespace internal

// then(sender, function) completes with function(value).
template <Sender SenderType, typename FunctionType>
  requires internal::NothrowInvocableWith<FunctionType, value_of_t<SenderType>>
class ThenSender final {
 public:
  using value_type =
      typename internal::ThenResult<FunctionType, value_of_t<SenderType>>::type;

  ThenSender(SenderType sender, FunctionType function)
      : sender_{std::move(sender)}, function_{std::move(function)} {}

  // The operation state is the inner sender's, connected to a receiver that
  // calls the function.
  template <Receiver<value_type> ReceiverType>
  auto connect(ReceiverType receiver) && {
    return std::move(sender_).connect(
        internal::ThenReceiver<FunctionType, ReceiverType>{
            std::move(function_), std::move(receiver)});
  }

 private:
  SenderType sender_;
  FunctionType function_;
};

template <Sender SenderType, typename FunctionType>
  requires internal::NothrowInvocableWith<FunctionType, value_of_t<SenderType>>
auto then(SenderType sender, FunctionType function)
    -> ThenSender<SenderType, FunctionType> {
  return {std::move(sender), std::move(function)};
}

// then(function), for `sender | then(function)`.
template <typename FunctionType>
struct ThenClosure final {
  FunctionType function;
};

template <typename FunctionType>
auto then(FunctionType function) -> ThenClosure<FunctionType> {
  return {std::move(function)};
}

template <Sender SenderType, typename FunctionType>
auto operator|(SenderType sender, ThenClosure<FunctionType> closure) {
  return then(std::move(sender), std::move(closure.function));
}

namespace internal {

// Where sync_wait's receiver puts the completion.
template <typename ValueType>
struct SyncWaitState final {
  std::optional<std::expected<ValueType, lib::Status>> result;
};

template <typename ValueType>
class SyncWaitReceiver final {
 public:
  explicit SyncWaitReceiver(Depend<SyncWaitState<ValueType>> state)
      : state_{state.get()} {}

  template <typename... ValueTypes>
  auto set_value(ValueTypes&&... values) && noexcept -> void {
    state_->result.emplace(std::in_place, std::forward<ValueTypes>(values)...);
  }

  auto set_error(lib::Status error) && noexcept -> void {
    state_->result.emplace(std::unexpect, std::move(error));
  }

  auto set_stopped() && noexcept -> void {
    state_->result.emplace(std::unexpect,
                           raise_io_error(-ECANCELED, "sync_wait"));
  }

 private:
  SyncWaitState<ValueType>* state_ = nullptr;
};

}  // namespace internal

// Connects `sender`, starts it, and runs `loop` until it completes. Returns its
// value, its error, IoError::CANCELLED if it stopped, or the loop's error if
// waiting failed for a reason other than a signal.
template <Sender SenderType>
  requires SenderTo<SenderType,
                    internal::SyncWaitReceiver<value_of_t<SenderType>>>
auto sync_wait(InOut<Loop> loop, SenderType&& sender)
    -> std::expected<value_of_t<SenderType>, lib::Status> {
  using ValueType = value_of_t<SenderType>;
  internal::SyncWaitState<ValueType> state;
  auto operation = std::forward<SenderType>(sender).connect(
      internal::SyncWaitReceiver<ValueType>{Depend(state)});
  operation.start();
  while (!state.result) {
    // A sender that waits on nothing the loop knows about would never finish.
    CHECK_INVARIANT(loop->pending() > 0);
    auto dispatched = loop->run_once();
    if (!dispatched && dispatched.error() != lib::watch(IoError::INTERRUPTED)) {
      return Failure{std::move(dispatched).error()};
    }
  }
  return *std::move(state.result);
}

}  // namespace iopia
