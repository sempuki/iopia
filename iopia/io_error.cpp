// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/io_error.hpp"
#include "core/status.hpp"

#include <cerrno>
#include <format>
#include <string>
#include <system_error>

template <>
const std::array<lib::StatusConditionEntry, iopia::IO_ERROR_COUNT>
    lib::EnumStatusKindConditionMixin<iopia::IoError,
                                      iopia::IO_ERROR_COUNT>::conditions_ = {
        lib::StatusConditionEntry{"io_uring unsupported"},
        lib::StatusConditionEntry{"io_uring denied"},
        lib::StatusConditionEntry{"resources exhausted"},
        lib::StatusConditionEntry{"kernel busy"},
        lib::StatusConditionEntry{"interrupted"},
        lib::StatusConditionEntry{"invalid argument"},
        lib::StatusConditionEntry{"cancelled"},
        lib::StatusConditionEntry{"system error"},
};

namespace iopia {
namespace {

auto condition_of(int error) -> IoError {
  switch (error) {
    case ENOSYS:
    case EOPNOTSUPP:
      return IoError::UNSUPPORTED;
    case EPERM:
    case EACCES:
      return IoError::DENIED;
    case ENOMEM:
    case EMFILE:
    case ENFILE:
      return IoError::EXHAUSTED;
    case EAGAIN:
    case EBUSY:
      return IoError::BUSY;
    case EINTR:
      return IoError::INTERRUPTED;
    case EINVAL:
    case EFAULT:
    case EBADF:
      return IoError::INVALID;
    case ECANCELED:
      return IoError::CANCELLED;
    default:
      return IoError::OTHER;
  }
}

}  // namespace

auto raise_io_error(int result, std::string_view call) -> Status {
  CHECK_PRECONDITION(result < 0);
  int error = -result;
  return lib::raise(condition_of(error),
                    std::format("{}: {} (errno {})", call,
                                std::generic_category().message(error), error));
}

auto raise_queue_full() -> Status {
  return lib::raise(IoError::BUSY,
                    "try_take_submission: no free submission queue entry");
}

}  // namespace iopia
