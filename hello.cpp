// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#include <liburing.h>

#include <print>

// Prints the version of liburing that iopia builds against, which shows that
// liburing compiles and links.
auto main() -> int {
  std::print("liburing {}.{}\n", io_uring_major_version(),
             io_uring_minor_version());
  return 0;
}
