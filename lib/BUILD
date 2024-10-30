load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

#-------------------------------------------------------------------------------

cc_library(
  name = "io_uring",
  hdrs= ["io_uring.hpp"],
)

#-------------------------------------------------------------------------------

cc_library(
  name = "testing",
  hdrs= ["testing.hpp"],
  srcs= ["testing.cpp"],
  deps = [
        "@catch2",
        "@catch2//:catch2_main",
  ],
)

