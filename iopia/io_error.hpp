// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <string_view>

#include "base/status.hpp"

// The errors iopia reports when a liburing call fails.
namespace iopia {

// The reasons a liburing call failed, grouped from the errno it returned. The
// message names the call and the errno.
enum class IoError {
  UNSUPPORTED,  // The kernel lacks io_uring, or the feature asked for.
  DENIED,       // io_uring is disabled, by sysctl or by a sandbox.
  EXHAUSTED,    // Out of memory, file descriptors or locked pages.
  BUSY,         // The kernel can't take more now; try again later.
  INTERRUPTED,  // A signal arrived while waiting.
  INVALID,      // An argument the kernel rejected.
  OTHER,        // Any other errno.
  COUNT,
};

inline constexpr std::size_t IO_ERROR_COUNT =
    static_cast<std::size_t>(IoError::COUNT);

// The error a fallible function returns.
using Failure = std::unexpected<lib::Status>;

// The status for `result`, a negative errno that liburing's `call` returned.
auto raise_io_error(int result, std::string_view call) -> lib::Status;

}  // namespace iopia

// Messages for each IoError, defined in io_error.cpp.
template <>
const std::array<lib::StatusConditionEntry, iopia::IO_ERROR_COUNT>
    lib::EnumStatusKindConditionMixin<iopia::IoError,
                                      iopia::IO_ERROR_COUNT>::conditions_;
