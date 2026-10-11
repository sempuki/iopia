// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/awaitable.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <utility>

#include "base/testing.hpp"
#include "iopia/task.hpp"

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

auto nop(Depend<Loop> loop) -> Task<std::expected<void, lib::Status>> {
  co_return co_await Nop{loop};
}

auto read(Depend<Loop> loop, std::int32_t file, std::span<std::byte> buffer)
    -> Task<std::expected<std::uint32_t, lib::Status>> {
  co_return co_await Read{loop, file, buffer, 0};
}

auto write_then_read(Depend<Loop> loop, std::int32_t file,
                     std::span<std::byte const> written,
                     std::span<std::byte> read)
    -> Task<std::expected<std::uint32_t, lib::Status>> {
  auto wrote = co_await Write{loop, file, written, 0};
  if (!wrote) {
    co_return Failure{std::move(wrote).error()};
  }
  co_return co_await Read{loop, file, read, 0};
}

auto timeout(Depend<Loop> loop, std::chrono::nanoseconds duration)
    -> Task<std::expected<void, lib::Status>> {
  co_return co_await Timeout{loop, duration};
}

auto prepare_nop(::io_uring_sqe* submission) noexcept -> void {
  ::io_uring_prep_nop(submission);
}

}  // namespace

TEST_CASE("Awaitable") {
  SECTION("ShouldSucceedGivenNop") {
    // Preconditions.
    Loop loop = make_loop();

    // Under Test.
    auto result = run(InOut(loop), nop(Depend(loop)));

    // Postconditions.
    REQUIRE(result);
    CHECK(*result);
  }

  SECTION("ShouldReadWhatWasWrittenGivenFile") {
    // Preconditions.
    Loop loop = make_loop();
    MemoryFile file;
    std::array<std::byte, 5> written;
    std::memcpy(written.data(), "hello", written.size());
    std::array<std::byte, 8> read{};

    // Under Test.
    auto result = run(
        InOut(loop), write_then_read(Depend(loop), file.file(), written, read));

    // Postconditions.
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == 5u);
    CHECK(std::memcmp(read.data(), "hello", 5) == 0);
  }

  SECTION("ShouldFailAsInvalidGivenBadFile") {
    // Preconditions.
    Loop loop = make_loop();
    std::array<std::byte, 8> buffer{};

    // Under Test.
    auto result = run(InOut(loop), read(Depend(loop), -1, buffer));

    // Postconditions.
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    CHECK(result->error() == lib::watch(IoError::INVALID));
  }

  SECTION("ShouldWaitAtLeastItsDurationGivenTimeout") {
    // Preconditions.
    Loop loop = make_loop();
    auto duration = std::chrono::milliseconds{2};
    auto start = std::chrono::steady_clock::now();

    // Under Test.
    auto result = run(InOut(loop), timeout(Depend(loop), duration));

    // Postconditions.
    REQUIRE(result);
    CHECK(*result);
    CHECK(std::chrono::steady_clock::now() - start >= duration);
  }

  SECTION("ShouldFailAsBusyWithoutSuspendingGivenFullQueueThatCantSubmit") {
    // Preconditions.
    // A disabled ring hands out entries but refuses to submit them.
    Loop loop = make_loop(1, IORING_SETUP_R_DISABLED);
    REQUIRE(loop.queue(prepare_nop, [](Completion) noexcept {}));

    // Under Test.
    auto result = run(InOut(loop), nop(Depend(loop)));

    // Postconditions.
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    CHECK(result->error() == lib::watch(IoError::BUSY));
  }
}

}  // namespace iopia
