// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/loop.hpp"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/testing.hpp"

namespace iopia {
namespace {

// An operation that counts its completions and keeps the last one.
struct CountingOperation final : Operation {
  CountingOperation() {
    complete = [](Operation& operation, Completion completion) noexcept {
      auto& self = static_cast<CountingOperation&>(operation);
      ++self.calls;
      self.last = completion;
    };
  }

  int calls = 0;
  Completion last;
};

auto make_loop(unsigned entries = 8, unsigned flags = 0) -> Loop {
  auto ring = Ring::make(entries, flags);
  REQUIRE(ring);
  return Loop{*std::move(ring)};
}

// Submits a NOP that completes `operation`.
auto submit_nop(lib::Depend<Operation> operation, lib::InOut<Loop> loop)
    -> void {
  ::io_uring_sqe* submission = loop->maybe_submission(operation);
  REQUIRE(submission);
  ::io_uring_prep_nop(submission);
}

// Prepares a NOP, for Loop::queue.
auto prepare_nop(::io_uring_sqe* submission) noexcept -> void {
  ::io_uring_prep_nop(submission);
}

// Whether Loop::queue accepts `PrepareType` and `HandlerType`.
template <typename PrepareType, typename HandlerType>
concept Queueable =
    requires(Loop& loop, PrepareType prepare, HandlerType handler) {
      loop.queue(std::move(prepare), std::move(handler));
    };

using CannotThrowPrepare = decltype(&prepare_nop);
using MayThrowPrepare = decltype([](::io_uring_sqe*) {});
using CannotThrow = decltype([](Completion) noexcept {});
using MayThrow = decltype([](Completion) {});

// A handler that may throw is refused at compile time, by both owners.
static_assert(Handler<CannotThrow>);
static_assert(!Handler<MayThrow>);
static_assert(Queueable<CannotThrowPrepare, CannotThrow>);
static_assert(!Queueable<CannotThrowPrepare, MayThrow>);

// So is a prepare function that may throw.
static_assert(Preparer<CannotThrowPrepare>);
static_assert(!Preparer<MayThrowPrepare>);
static_assert(!Queueable<MayThrowPrepare, CannotThrow>);

// An operation's complete function can't throw either.
using MayThrowComplete = decltype([](Operation&, Completion) {});
static_assert(
    !std::is_convertible_v<MayThrowComplete, Operation::CompleteFunction>);

// Runs `loop` until nothing is pending.
auto run_until_idle(lib::InOut<Loop> loop) -> void {
  while (loop->pending() > 0) {
    REQUIRE(loop->run_once());
  }
}

}  // namespace

TEST_CASE("Loop") {
  SECTION("ShouldCompleteEachOperationGivenTwoNops") {
    // Preconditions.
    Loop loop = make_loop();
    CountingOperation first;
    CountingOperation second;
    submit_nop(lib::Depend(first), lib::InOut(loop));
    submit_nop(lib::Depend(second), lib::InOut(loop));

    // Under Test.
    auto dispatched = loop.run_once();

    // Postconditions.
    REQUIRE(dispatched);
    CHECK(*dispatched == 2u);
    CHECK(first.calls == 1);
    CHECK(second.calls == 1);
    CHECK(first.last.result == 0);
    CHECK(first.last.user_data == reinterpret_cast<std::uintptr_t>(&first));
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldCompleteAgainGivenOperationResubmittedFromItsHandler") {
    // Preconditions.
    // An operation that submits itself once more from its own handler.
    struct RepeatingOperation final : Operation {
      explicit RepeatingOperation(lib::Depend<Loop> loop) : loop_{loop.get()} {
        complete = [](Operation& operation, Completion) noexcept {
          auto& self = static_cast<RepeatingOperation&>(operation);
          if (++self.calls == 1) {
            submit_nop(lib::Depend(self), lib::InOut(*self.loop_));
          }
        };
      }

      Loop* loop_ = nullptr;
      int calls = 0;
    };
    Loop loop = make_loop();
    RepeatingOperation operation{lib::Depend(loop)};
    submit_nop(lib::Depend(operation), lib::InOut(loop));

    // Under Test.
    auto first = loop.run_once();
    auto second = loop.run_once();

    // Postconditions.
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first == 1u);
    CHECK(*second == 1u);
    CHECK(operation.calls == 2);
  }

  SECTION("ShouldReturnZeroWithoutWaitingGivenNothingPending") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto dispatched = loop.run_once();

    // Postconditions.
    REQUIRE(dispatched);
    CHECK(*dispatched == 0u);
  }

  SECTION("ShouldFailContractGivenOperationWithoutCompleteFunction") {
    // Preconditions.
    Loop loop = make_loop();
    Operation operation;

    // Under Test.
    // Postconditions.
    CHECK_THROWS_AS(loop.maybe_submission(lib::Depend(operation)),
                    std::logic_error);
  }

  SECTION("ShouldCallCallerOwnedHandlerGivenNop") {
    // Preconditions.
    Loop loop = make_loop();
    std::vector<Completion> completions;
    HandlerOperation operation{[&](Completion completion) noexcept {
      completions.push_back(completion);
    }};
    submit_nop(lib::Depend(operation), lib::InOut(loop));

    // Under Test.
    auto dispatched = loop.run_once();

    // Postconditions.
    REQUIRE(dispatched);
    REQUIRE(completions.size() == 1u);
    CHECK(completions[0].result == 0);
  }

  SECTION("ShouldCallLoopOwnedHandlerOnceGivenNop") {
    // Preconditions.
    Loop loop = make_loop();
    std::vector<Completion> completions;
    REQUIRE(loop.queue(prepare_nop, [&](Completion completion) noexcept {
      completions.push_back(completion);
    }));

    // Under Test.
    auto dispatched = loop.run_once();

    // Postconditions.
    REQUIRE(dispatched);
    CHECK(*dispatched == 1u);
    REQUIRE(completions.size() == 1u);
    CHECK(completions[0].result == 0);
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldDestroyLoopOwnedHandlerGivenItsLastCompletion") {
    // Preconditions.
    Loop loop = make_loop();
    auto captured = std::make_shared<int>(0);
    REQUIRE(loop.queue(prepare_nop,
                       [captured](Completion) noexcept { ++*captured; }));
    REQUIRE(captured.use_count() == 2);

    // Under Test.
    REQUIRE(loop.run_once());

    // Postconditions.
    CHECK(*captured == 1);
    CHECK(captured.use_count() == 1);
  }

  SECTION("ShouldSubmitAndQueueGivenFullQueue") {
    // Preconditions.
    Loop loop = make_loop(1);
    int calls = 0;
    REQUIRE(loop.queue(prepare_nop, [&](Completion) noexcept { ++calls; }));

    // Under Test.
    bool queued =
        loop.queue(prepare_nop, [&](Completion) noexcept { ++calls; });

    // Postconditions.
    CHECK(queued);
    CHECK(loop.pending() == 2u);
    run_until_idle(lib::InOut(loop));
    CHECK(calls == 2);
  }

  SECTION("ShouldRefuseLoopOwnedHandlerGivenFullQueueThatCantSubmit") {
    // Preconditions.
    // A disabled ring hands out entries but refuses to submit them.
    Loop loop = make_loop(1, IORING_SETUP_R_DISABLED);
    REQUIRE(loop.queue(prepare_nop, [](Completion) noexcept {}));

    // Under Test.
    bool queued = loop.queue(prepare_nop, [](Completion) noexcept {});

    // Postconditions.
    CHECK_FALSE(queued);
    CHECK(loop.pending() == 1u);
  }

  SECTION("ShouldDispatchEveryCompletionGivenMoreThanOneBatch") {
    // Preconditions.
    constexpr int COUNT = 100;
    Loop loop = make_loop(128);
    int calls = 0;
    for (int i = 0; i < COUNT; ++i) {
      REQUIRE(loop.queue(prepare_nop, [&](Completion) noexcept { ++calls; }));
    }

    // Under Test.
    auto dispatched = loop.run_once();

    // Postconditions.
    REQUIRE(dispatched);
    CHECK(*dispatched == unsigned{COUNT});
    CHECK(calls == COUNT);
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldRunUntilNothingPendingGivenHandlersThatQueueMore") {
    // Preconditions.
    Loop loop = make_loop();
    int calls = 0;
    // Each handler queues the next, three deep.
    auto queue_next = [&](this auto const& self) -> void {
      REQUIRE(loop.queue(prepare_nop, [&, self](Completion) noexcept {
        if (++calls < 3) {
          self();
        }
      }));
    };
    queue_next();

    // Under Test.
    auto ran = loop.run();

    // Postconditions.
    REQUIRE(ran);
    CHECK(calls == 3);
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldReturnFromRunGivenHandlerCallsStop") {
    // Preconditions.
    Loop loop = make_loop();
    ::__kernel_timespec interval{.tv_sec = 0, .tv_nsec = 1'000'000};
    int calls = 0;
    // A multishot timeout with no count fires until it is cancelled.
    REQUIRE(loop.queue(
        [&](::io_uring_sqe* submission) noexcept {
          ::io_uring_prep_timeout(submission, &interval, 0,
                                  IORING_TIMEOUT_MULTISHOT);
        },
        [&](Completion) noexcept {
          ++calls;
          loop.stop();
        }));

    // Under Test.
    auto first = loop.run();
    auto second = loop.run();

    // Postconditions.
    REQUIRE(first);
    REQUIRE(second);
    CHECK(calls == 2);
    CHECK(loop.pending() == 1u);
  }

  SECTION("ShouldCallHandlerUntilLastCompletionGivenMultishotTimeout") {
    // Preconditions.
    Loop loop = make_loop();
    ::__kernel_timespec interval{.tv_sec = 0, .tv_nsec = 1'000'000};
    std::vector<Completion> completions;
    REQUIRE(loop.queue(
        [&](::io_uring_sqe* submission) noexcept {
          ::io_uring_prep_timeout(submission, &interval, 3,
                                  IORING_TIMEOUT_MULTISHOT);
        },
        [&](Completion completion) noexcept {
          completions.push_back(completion);
        }));

    // Under Test.
    run_until_idle(lib::InOut(loop));

    // Postconditions.
    REQUIRE(completions.size() == 3u);
    CHECK(completions[0].flags & IORING_CQE_F_MORE);
    CHECK(completions[1].flags & IORING_CQE_F_MORE);
    CHECK_FALSE(completions[2].flags & IORING_CQE_F_MORE);
    CHECK(completions[2].result == -ETIME);
  }

  SECTION("ShouldCancelPendingOperationGivenLoopDestroyed") {
    // Preconditions.
    ::__kernel_timespec long_wait{.tv_sec = 60, .tv_nsec = 0};
    std::vector<Completion> completions;
    auto ring = Ring::make(8);
    REQUIRE(ring);
    auto loop = std::make_unique<Loop>(*std::move(ring));
    REQUIRE(loop->queue(
        [&](::io_uring_sqe* submission) noexcept {
          ::io_uring_prep_timeout(submission, &long_wait, 0, 0);
        },
        [&](Completion completion) noexcept {
          completions.push_back(completion);
        }));
    REQUIRE(loop->submit());

    // Under Test.
    loop.reset();

    // Postconditions.
    REQUIRE(completions.size() == 1u);
    CHECK(completions[0].result == -ECANCELED);
  }
}

}  // namespace iopia
