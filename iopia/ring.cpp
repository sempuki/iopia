// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include "iopia/ring.hpp"

namespace iopia {

auto internal::IoUringDelete::operator()(::io_uring* ring) const noexcept
    -> void {
  ::io_uring_queue_exit(ring);
  delete ring;
}

auto Ring::make(std::uint32_t entries, std::uint32_t flags)
    -> std::expected<Ring, lib::Status> {
  auto ring = std::make_unique<::io_uring>();
  if (int result = ::io_uring_queue_init(entries, ring.get(), flags);
      result < 0) {
    return Failure{raise_io_error(result, "io_uring_queue_init")};
  }
  return Ring{ring.release()};
}

}  // namespace iopia
