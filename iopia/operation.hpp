// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <utility>

#include "base/core.hpp"
#include "iopia/ring.hpp"

// What the loop knows of a pending operation, and the operation that wraps a
// callable its caller owns.
namespace iopia {

// The base of every operation. The loop knows only the function to call when
// one of the operation's completions arrives, and calls it with the operation
// itself and the completion. An operation type derives from Operation, sets
// `complete` to a function that casts the Operation back to its own type, and
// stays alive at the same address until its last completion is dispatched.
struct Operation {
  using CompleteFunction = auto (*)(Operation& operation, Completion completion)
      -> void;

  CompleteFunction complete = nullptr;
};

// An operation that calls a callable its caller owns with each completion. It
// can be neither copied nor moved, so it stays at the address the kernel
// holds.
template <typename CallableType>
class CallableOperation final : public Operation {
 public:
  DECLARE_COPY_DELETE(CallableOperation);
  DECLARE_MOVE_DELETE(CallableOperation);

  explicit CallableOperation(CallableType callable)
      : callable_{std::move(callable)} {
    complete = [](Operation& operation, Completion completion) {
      static_cast<CallableOperation&>(operation).callable_(completion);
    };
  }

  ~CallableOperation() = default;

 private:
  CallableType callable_;
};

}  // namespace iopia
