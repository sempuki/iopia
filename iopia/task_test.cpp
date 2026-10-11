// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/task.hpp"

#include <cstdint>
#include <memory>
#include <utility>

#include "base/testing.hpp"
#include "iopia/awaitable.hpp"

namespace iopia {
namespace {

auto make_loop() -> Loop {
  auto ring = Ring::make(8);
  REQUIRE(ring);
  return Loop{*std::move(ring)};
}

auto forty_two() -> Task<std::int32_t> { co_return 42; }

auto after_nop(Depend<Loop> loop) -> Task<std::int32_t> {
  auto nop = co_await Nop{loop};
  CHECK(nop);
  co_return 7;
}

auto doubled(Depend<Loop> loop) -> Task<std::int32_t> {
  std::int32_t value = co_await after_nop(loop);
  co_return value * 2;
}

auto count_nops(Depend<Loop> loop, std::int32_t count,
                Depend<std::int32_t> done) -> Task<void> {
  for (std::int32_t index = 0; index < count; ++index) {
    auto nop = co_await Nop{loop};
    CHECK(nop);
    ++*done;
  }
}

auto sum_of_ones(std::int32_t count) -> Task<std::int64_t> {
  std::int64_t sum = 0;
  for (std::int32_t index = 0; index < count; ++index) {
    sum += co_await forty_two() / 42;
  }
  co_return sum;
}

auto hold(std::shared_ptr<std::int32_t> held) -> Task<void> {
  ++*held;
  co_return;
}

}  // namespace

TEST_CASE("Task") {
  SECTION("ShouldReturnValueGivenTaskThatDoesntSuspend") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value = run(InOut(loop), forty_two());

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 42);
  }

  SECTION("ShouldReturnValueGivenTaskThatAwaitsNop") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value = run(InOut(loop), after_nop(Depend(loop)));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 7);
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldReturnChildsValueGivenNestedTasks") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value = run(InOut(loop), doubled(Depend(loop)));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 14);
  }

  SECTION("ShouldRunToTheEndGivenVoidTask") {
    // Preconditions.
    Loop loop = make_loop();
    std::int32_t done = 0;

    // Under Test.
    auto ran = run(InOut(loop), count_nops(Depend(loop), 3, Depend(done)));

    // Postconditions.
    REQUIRE(ran);
    CHECK(done == 3);
  }

  SECTION("ShouldNotGrowStackGivenManyChildrenThatDontSuspend") {
    // Preconditions.
    // Each child finishes before it suspends. If finishing resumed the parent
    // one frame deeper each time, this many would overflow the stack.
    Loop loop = make_loop();

    // Under Test.
    auto sum = run(InOut(loop), sum_of_ones(1'000'000));

    // Postconditions.
    REQUIRE(sum);
    CHECK(*sum == 1'000'000);
  }

  SECTION("ShouldNotStartGivenTaskDestroyedUnawaited") {
    // Preconditions.
    auto held = std::make_shared<std::int32_t>(0);

    // Under Test.
    {
      auto task = hold(held);
    }

    // Postconditions.
    CHECK(*held == 0);
    CHECK(held.use_count() == 1);
  }

  SECTION("ShouldRunMovedTaskOnceGivenMove") {
    // Preconditions.
    Loop loop = make_loop();
    auto held = std::make_shared<std::int32_t>(0);
    Task<void> first = hold(held);
    Task<void> second = std::move(first);

    // Under Test.
    auto ran = run(InOut(loop), std::move(second));

    // Postconditions.
    REQUIRE(ran);
    CHECK(*held == 1);
    CHECK(held.use_count() == 1);
  }
}

}  // namespace iopia
