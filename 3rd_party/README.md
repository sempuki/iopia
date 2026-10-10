# Third-party software

Everything iopia takes from outside its own code comes through this directory.
BUILD files depend on libraries by `//3rd_party:<name>`.

## Libraries

From the [Bazel Central Registry](https://registry.bazel.build), at the
versions `MODULE.bazel` pins.

| Target | Library | Version | License | Used by |
|---|---|---|---|---|
| `liburing` | [liburing](https://github.com/axboe/liburing) | 2.15 | MIT or LGPL-2.1 | everything |

The registry builds liburing from source, but its configure step reads the
host's kernel headers to write a compatibility header. The built library still
runs on other kernels, and reports what a kernel lacks when a ring is set up or
an operation is submitted.

[Catch2](https://github.com/catchorg/Catch2) runs the tests, through
`@lib//base:testing` in `2nd_party/lib`.
