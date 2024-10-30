# When bzlmod is enabled, this file is unused; WORKSPACE.bzlmod is used instead.
workspace(name = "iopia")

# Fetches are order-independent, so we declare those first.
# All http_archive, http_file, etc. rules should be in the repositories.bzl file.
# DO NOT load those functions here in WORKSPACE.
load("//third_party:repositories.bzl", "fetch_deps")

fetch_deps()

# The remainder of this file is highly order-dependent. As much as possible, the fetch_deps
# call above has already pinned our dependencies to the exact version we want.
#
# If possible, migrate to bzlmod rather than struggle with the implications of moving lines
# and getting different dependency verisons.
#
# For each of our direct dependencies:
#   - load and run their dependency loader method.
#   - register any toolchains they might provide which we intend to use.

load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")

bazel_skylib_workspace()
