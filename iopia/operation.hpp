// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <type_traits>
#include <utility>

#include "base/core.hpp"
#include "iopia/ring.hpp"

// The base of every operation the loop dispatches, and the operation that
// wraps a handler its caller owns.
namespace iopia {

// The base of every operation. The loop knows only the function to call when
// one of the operation's completions arrives, and calls it with the operation
// itself and the completion. An operation type derives from Operation, sets
// `complete` to a noexcept function that casts the Operation back to its own
// type, and stays alive at the same address until its last completion is
// dispatched.
struct Operation {
  using CompleteFunction = auto (*)(Operation& operation,
                                    Completion completion) noexcept -> void;

  CompleteFunction complete = nullptr;
};

// A completion handler is a callable that takes each Completion and can't
// throw.
template <typename HandlerType>
concept Handler = std::is_nothrow_invocable_v<HandlerType&, Completion>;

// An operation that calls a handler its caller owns with each completion. It
// can be neither copied nor moved, so it stays at the address the kernel
// holds.
template <Handler HandlerType>
class HandlerOperation final : public Operation {
 public:
  DECLARE_COPY_DELETE(HandlerOperation);
  DECLARE_MOVE_DELETE(HandlerOperation);

  explicit HandlerOperation(HandlerType handler)
      : handler_{std::move(handler)} {
    complete = [](Operation& operation, Completion completion) noexcept {
      static_cast<HandlerOperation&>(operation).handler_(completion);
    };
  }

  ~HandlerOperation() = default;

 private:
  HandlerType handler_;
};

}  // namespace iopia
