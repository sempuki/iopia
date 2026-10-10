// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/ring.hpp"

#include <array>
#include <cstdint>
#include <utility>

#include "base/testing.hpp"

namespace iopia {

TEST_CASE("Ring") {
  SECTION("ShouldCompleteNopWithItsUserDataGivenOneSubmission") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);
    ::io_uring_sqe* submission = ring->maybe_submission();
    REQUIRE(submission);
    ::io_uring_prep_nop(submission);
    ::io_uring_sqe_set_data64(submission, 42);

    // Under Test.
    auto submitted = ring->submit();
    auto completion = ring->wait();

    // Postconditions.
    REQUIRE(submitted);
    CHECK(*submitted == 1u);
    REQUIRE(completion);
    CHECK(completion->user_data == 42u);
    CHECK(completion->result == 0);
  }

  SECTION("ShouldReturnNullSubmissionGivenFullQueue") {
    // Preconditions.
    auto ring = Ring::make(1);
    REQUIRE(ring);
    REQUIRE(ring->maybe_submission());

    // Under Test.
    ::io_uring_sqe* submission = ring->maybe_submission();

    // Postconditions.
    CHECK(submission == nullptr);
  }

  SECTION("ShouldKeepItsIoUringGivenMove") {
    // Preconditions.
    auto made = Ring::make(8);
    REQUIRE(made);

    // Under Test.
    Ring ring = *std::move(made);

    // Postconditions.
    ::io_uring_sqe* submission = ring.maybe_submission();
    REQUIRE(submission);
    ::io_uring_prep_nop(submission);
    ::io_uring_sqe_set_data64(submission, 7);
    REQUIRE(ring.submit());
    auto completion = ring.wait();
    REQUIRE(completion);
    CHECK(completion->user_data == 7u);
  }

  SECTION("ShouldReturnNoCompletionGivenNothingSubmitted") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);

    // Under Test.
    auto completion = ring->maybe_completion();

    // Postconditions.
    CHECK_FALSE(completion);
  }

  SECTION("ShouldReadyEveryCompletionGivenSubmitAndWaitForAll") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);
    for (std::uint64_t user_data : {1u, 2u}) {
      ::io_uring_sqe* submission = ring->maybe_submission();
      REQUIRE(submission);
      ::io_uring_prep_nop(submission);
      ::io_uring_sqe_set_data64(submission, user_data);
    }

    // Under Test.
    auto submitted = ring->submit_and_wait(2);

    // Postconditions.
    REQUIRE(submitted);
    CHECK(*submitted == 2u);
    auto first = ring->maybe_completion();
    auto second = ring->maybe_completion();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->user_data + second->user_data == 3u);
    CHECK_FALSE(ring->maybe_completion());
  }

  SECTION("ShouldTakeEveryReadyCompletionInOrderGivenRoomForAll") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);
    for (std::uint64_t user_data : {1u, 2u, 3u}) {
      ::io_uring_sqe* submission = ring->maybe_submission();
      REQUIRE(submission);
      ::io_uring_prep_nop(submission);
      ::io_uring_sqe_set_data64(submission, user_data);
    }
    REQUIRE(ring->submit_and_wait(3));
    std::array<Completion, 8> into;

    // Under Test.
    std::size_t taken = ring->take_completions(into);

    // Postconditions.
    REQUIRE(taken == 3u);
    CHECK(into[0].user_data == 1u);
    CHECK(into[1].user_data == 2u);
    CHECK(into[2].user_data == 3u);
    CHECK_FALSE(ring->maybe_completion());
  }

  SECTION("ShouldLeaveTheRestReadyGivenRoomForFewer") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);
    for (std::uint64_t user_data : {1u, 2u, 3u}) {
      ::io_uring_sqe* submission = ring->maybe_submission();
      REQUIRE(submission);
      ::io_uring_prep_nop(submission);
      ::io_uring_sqe_set_data64(submission, user_data);
    }
    REQUIRE(ring->submit_and_wait(3));
    std::array<Completion, 2> into;

    // Under Test.
    std::size_t first = ring->take_completions(into);
    std::uint64_t first_user_data = into[0].user_data;
    std::size_t second = ring->take_completions(into);

    // Postconditions.
    CHECK(first == 2u);
    CHECK(first_user_data == 1u);
    REQUIRE(second == 1u);
    CHECK(into[0].user_data == 3u);
  }

  SECTION("ShouldTakeNothingGivenNothingSubmitted") {
    // Preconditions.
    auto ring = Ring::make(8);
    REQUIRE(ring);
    std::array<Completion, 4> into;

    // Under Test.
    std::size_t taken = ring->take_completions(into);

    // Postconditions.
    CHECK(taken == 0u);
  }

  SECTION("ShouldFailAsInvalidGivenZeroEntries") {
    // Under Test.
    auto ring = Ring::make(0);

    // Postconditions.
    REQUIRE_FALSE(ring);
    CHECK(ring.error().kind() == lib::watch(IoError::INVALID));
  }
}

}  // namespace iopia
