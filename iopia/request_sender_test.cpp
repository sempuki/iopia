// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/request_sender.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "base/testing.hpp"

namespace iopia {
namespace {

auto make_loop(std::uint32_t entries = 8, std::uint32_t flags = 0) -> Loop {
  auto ring = Ring::make(entries, flags);
  REQUIRE(ring);
  return Loop{*std::move(ring)};
}

// An in-memory file, closed when it goes out of scope.
class MemoryFile final {
 public:
  DECLARE_COPY_DELETE(MemoryFile);
  DECLARE_MOVE_DELETE(MemoryFile);

  MemoryFile() : file_{::memfd_create("iopia-test", 0)} { REQUIRE(file_ >= 0); }
  ~MemoryFile() { ::close(file_); }

  auto file() const -> std::int32_t { return file_; }

 private:
  std::int32_t file_ = -1;
};

// A receiver of void that records which completion it got.
struct RecordingReceiver final {
  enum class Completion { NONE, VALUE, ERROR, STOPPED };

  auto set_value() && noexcept -> void { *completion = Completion::VALUE; }
  auto set_error(lib::Status) && noexcept -> void {
    *completion = Completion::ERROR;
  }
  auto set_stopped() && noexcept -> void { *completion = Completion::STOPPED; }

  Completion* completion = nullptr;
};

auto prepare_no_op(::io_uring_sqe* submission) noexcept -> void {
  ::io_uring_prep_nop(submission);
}

static_assert(Sender<ReadSender>);
static_assert(SenderTo<NoOpSender, RecordingReceiver>);
static_assert(!SenderTo<ReadSender, RecordingReceiver>);

}  // namespace

TEST_CASE("RequestSender") {
  SECTION("ShouldSucceedGivenNoOpSender") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto done = sync_wait(InOut(loop), NoOpSender{Depend(loop)});

    // Postconditions.
    REQUIRE(done);
    CHECK(loop.pending() == 0u);
  }

  SECTION("ShouldReadWhatWasWrittenGivenReadAndWriteSenders") {
    // Preconditions.
    Loop loop = make_loop();
    MemoryFile file;
    std::array<std::byte, 5> written;
    std::memcpy(written.data(), "hello", written.size());
    std::array<std::byte, 8> read{};

    // Under Test.
    auto wrote = sync_wait(InOut(loop),
                           WriteSender{Depend(loop), file.file(),
                                       std::span<std::byte const>{written}, 0});
    auto bytes = sync_wait(
        InOut(loop), ReadSender{Depend(loop), file.file(), std::span{read}, 0});

    // Postconditions.
    REQUIRE(wrote);
    CHECK(*wrote == 5u);
    REQUIRE(bytes);
    CHECK(*bytes == 5u);
    CHECK(std::memcmp(read.data(), "hello", 5) == 0);
  }

  SECTION("ShouldApplyThenGivenReadSender") {
    // Preconditions.
    Loop loop = make_loop();
    MemoryFile file;
    REQUIRE(::write(file.file(), "hello", 5) == 5);
    std::array<std::byte, 8> buffer{};

    // Under Test.
    auto doubled = sync_wait(
        InOut(loop),
        ReadSender{Depend(loop), file.file(), std::span{buffer}, 0} |
            then([](std::uint32_t bytes) noexcept { return bytes * 2; }));

    // Postconditions.
    REQUIRE(doubled);
    CHECK(*doubled == 10u);
  }

  SECTION("ShouldFailAsInvalidWithoutCallingThenGivenBadFile") {
    // Preconditions.
    Loop loop = make_loop();
    std::array<std::byte, 8> buffer{};
    bool called = false;

    // Under Test.
    auto result = sync_wait(
        InOut(loop), ReadSender{Depend(loop), -1, std::span{buffer}, 0} |
                         then([&](std::uint32_t) noexcept { called = true; }));

    // Postconditions.
    REQUIRE_FALSE(result);
    CHECK(result.error() == lib::watch(IoError::INVALID));
    CHECK_FALSE(called);
  }

  SECTION("ShouldWaitAtLeastItsDurationGivenTimeoutSender") {
    // Preconditions.
    Loop loop = make_loop();
    auto duration = std::chrono::milliseconds{2};
    auto start = std::chrono::steady_clock::now();

    // Under Test.
    auto done = sync_wait(InOut(loop), TimeoutSender{Depend(loop), duration});

    // Postconditions.
    REQUIRE(done);
    CHECK(std::chrono::steady_clock::now() - start >= duration);
  }

  SECTION("ShouldRunOnTheLoopGivenSchedulerAndThen") {
    // Preconditions.
    Loop loop = make_loop();
    LoopScheduler scheduler{Depend(loop)};

    // Under Test.
    auto value = sync_wait(
        InOut(loop), scheduler.schedule() | then([]() noexcept { return 42; }));

    // Postconditions.
    REQUIRE(value);
    CHECK(*value == 42);
    CHECK(scheduler == LoopScheduler{Depend(loop)});
  }

  SECTION("ShouldFailAsBusyAtOnceGivenFullQueueThatCantSubmit") {
    // Preconditions.
    // A disabled ring hands out entries but refuses to submit them.
    Loop loop = make_loop(1, IORING_SETUP_R_DISABLED);
    REQUIRE(loop.queue(prepare_no_op, [](Completion) noexcept {}));

    // Under Test.
    auto result = sync_wait(InOut(loop), NoOpSender{Depend(loop)});

    // Postconditions.
    REQUIRE_FALSE(result);
    CHECK(result.error() == lib::watch(IoError::BUSY));
  }

  SECTION("ShouldCompleteValueGivenReceiverConnectedAndStartedByHand") {
    // Preconditions.
    auto completion = RecordingReceiver::Completion::NONE;
    Loop loop = make_loop();
    auto operation =
        NoOpSender{Depend(loop)}.connect(RecordingReceiver{&completion});

    // Under Test.
    operation.start();
    REQUIRE(loop.run());

    // Postconditions.
    CHECK(completion == RecordingReceiver::Completion::VALUE);
  }

  SECTION("ShouldStopGivenLoopDestroyedWhilePending") {
    // Preconditions.
    // The loop's destructor cancels the timeout, which completes the receiver.
    auto completion = RecordingReceiver::Completion::NONE;
    auto ring = Ring::make(8);
    REQUIRE(ring);
    auto loop = std::make_unique<Loop>(*std::move(ring));
    auto operation =
        TimeoutSender{Depend(*loop), std::chrono::seconds{60}}.connect(
            RecordingReceiver{&completion});
    operation.start();
    REQUIRE(loop->submit());

    // Under Test.
    loop.reset();

    // Postconditions.
    CHECK(completion == RecordingReceiver::Completion::STOPPED);
  }
}

}  // namespace iopia
