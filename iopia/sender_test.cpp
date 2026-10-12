// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/sender.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "base/testing.hpp"
#include "iopia/request_sender.hpp"

namespace iopia {
namespace {

auto make_loop() -> Loop {
  auto ring = Ring::make(8);
  REQUIRE(ring);
  return Loop{*std::move(ring)};
}

// A receiver of std::int32_t that keeps what it was given.
struct KeepingReceiver final {
  auto set_value(std::int32_t value) && noexcept -> void { *kept = value; }
  auto set_error(lib::Status) && noexcept -> void {}
  auto set_stopped() && noexcept -> void {}

  std::int32_t* kept = nullptr;
};

// A receiver that can throw from set_value.
struct ThrowingReceiver final {
  auto set_value(std::int32_t) && -> void {}
  auto set_error(lib::Status) && noexcept -> void {}
  auto set_stopped() && noexcept -> void {}
};

static_assert(Receiver<KeepingReceiver, std::int32_t>);
static_assert(!Receiver<ThrowingReceiver, std::int32_t>);
static_assert(!Receiver<KeepingReceiver, void>);
static_assert(Sender<JustSender<std::int32_t>>);
static_assert(!Sender<std::int32_t>);
static_assert(SenderTo<JustSender<std::int32_t>, KeepingReceiver>);
static_assert(Scheduler<LoopScheduler>);
static_assert(!Scheduler<JustSender<std::int32_t>>);

using MayThrowFunction = decltype([](std::int32_t value) { return value; });
static_assert(!internal::NothrowInvocableWith<MayThrowFunction, std::int32_t>);

}  // namespace

TEST_CASE("Sender") {
  SECTION("ShouldCompleteWithValueGivenJustConnectedAndStarted") {
    // Preconditions.
    std::int32_t kept = 0;
    auto operation = just(std::int32_t{42}).connect(KeepingReceiver{&kept});

    // Under Test.
    operation.start();

    // Postconditions.
    CHECK(kept == 42);
  }

  SECTION("ShouldReturnValueGivenSyncWaitOnJust") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value = sync_wait(InOut(loop), just(std::int32_t{7}));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 7);
  }

  SECTION("ShouldApplyFunctionGivenThen") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value =
        sync_wait(InOut(loop),
                  then(just(std::int32_t{7}),
                       [](std::int32_t value) noexcept { return value * 2; }));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 14);
  }

  SECTION("ShouldChainFunctionsGivenThenThroughPipes") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto value =
        sync_wait(InOut(loop),
                  just(std::int32_t{7}) | then([](std::int32_t value) noexcept {
                    return value + 1;
                  }) | then([](std::int32_t value) noexcept {
                    return std::to_string(value);
                  }));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == "8");
  }

  SECTION("ShouldCompleteWithVoidGivenThenReturningNothing") {
    // Preconditions.
    Loop loop = make_loop();
    std::int32_t seen = 0;

    // Under Test.
    auto done =
        sync_wait(InOut(loop),
                  just(std::int32_t{3}) |
                      then([&](std::int32_t value) noexcept { seen = value; }));

    // Postconditions.
    REQUIRE(done);
    CHECK(seen == 3);
  }
}

}  // namespace iopia
