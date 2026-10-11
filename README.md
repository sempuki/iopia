# iopia

C++ front-ends for Linux's io_uring, coroutines and senders and receivers,
both modeled on the C++ standard's designs and built on one ring and one
completion protocol. On top of them, a gateway that moves messages between
local publish-subscribe systems, such as iceoryx2, and the network.

How to use iopia is in [Using.md](Using.md). The design, its decisions and
the plan are in [Design.md](Design.md), and
[Design.coroutine.md](Design.coroutine.md) explains the coroutine front-end.

## Build

Requires Linux and [Bazelisk](https://github.com/bazelbuild/bazelisk)
(installed as `bazel`); the Bazel release is pinned in `.bazelversion`.
liburing comes from the Bazel Central Registry, and the shared core libraries
from the [lib](https://github.com/sempuki/lib) submodule:

```sh
git clone --recurse-submodules git@github.com:sempuki/iopia.git
# or, in an existing clone:
git submodule update --init

bazel test //...
bazel run //:hello   # print the liburing version iopia links
```

Code targets C++26; flags come from `@lib//bazel:copts.bzl`. Put machine-local
flags in an untracked `user.bazelrc`.

## Sanitizers

Run the tests under AddressSanitizer and UndefinedBehaviorSanitizer with
Clang, which ships its own runtimes. Only iopia's sources are instrumented,
and the tests link statically, because liburing's shared library doesn't link
with sanitizer flags:

```sh
CC=clang CXX=clang++ bazel test //... \
  --repo_env=CC=clang --repo_env=CXX=clang++ \
  --build_tests_only --dynamic_mode=off \
  --per_file_copt='iopia/.*@-fsanitize=address' \
  --per_file_copt='iopia/.*@-fsanitize=undefined' \
  --linkopt=-fsanitize=address --linkopt=-fsanitize=undefined
```

`--per_file_copt` splits its value at commas, so each sanitizer gets its own
flag.

## Editor setup

clangd needs a `compile_commands.json`. lib's `bazel/lsp_mirror.py` builds in
an output base of its own, copies the headers clangd reads into `.lsp/mirror/`
(ignored by git and Bazel), and writes `compile_commands.json` against that
mirror:

```sh
python3 2nd_party/lib/bazel/lsp_mirror.py                  # build the mirror now
python3 2nd_party/lib/bazel/lsp_mirror.py --if-stale       # only if anything changed
python3 2nd_party/lib/bazel/lsp_mirror.py --install-hooks  # after checkout, merge, rebase
```

Restart clangd (`:LspRestart` in Neovim) after the first build.

## Credits

| Library | Used for | License |
|---|---|---|
| [liburing](https://github.com/axboe/liburing) | The io_uring system calls and ring helpers | MIT or LGPL-2.1 |
| [Catch2](https://github.com/catchorg/Catch2) | Tests | BSL-1.0 |

## License

iopia is licensed under the GNU General Public License, version 3 only
(GPL-3.0-only). See [LICENSE](LICENSE).
