"""External repositories, fetched by Bazel during analysis, when bzlmod is disabled.

Unlike the rest of /WORKSPACE, these calls are guaranteed to be order-independent.
We also put all fetches in this file to make WORKSPACE shorter.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", _http_archive = "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

# Wrap http_archive with maybe so we don't try to declare a dependency twice
def http_archive(**kwargs):
    maybe(_http_archive, **kwargs)

def fetch_deps():
    """Definitions of external fetches.

    Bazel will only lazy-download the ones needed for requested targets.
    """
    http_archive(
        name = "bazel_skylib",
        sha256 = "bc283cdfcd526a52c3201279cda4bc298652efa898b10b4db0837dc51652756f",
        urls = [
            "https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/releases/download/1.7.1/bazel-skylib-1.7.1.tar.gz",
            "https://github.com/bazelbuild/bazel-skylib/releases/download/1.7.1/bazel-skylib-1.7.1.tar.gz",
        ],
    )

    http_archive(
        name = "catch2",
        url = "https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.zip",
        sha256 = "",
        strip_prefix = "Catch2-3.7.1",
    )
