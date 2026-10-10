# iopia design

iopia gives Linux's io_uring two C++ front-ends, C++20 coroutines and senders
and receivers, both built on one ring and one completion protocol.

This document records the architecture, the decisions behind it, and the
roadmap. Update it when a decision changes.

## Goals

In priority order:

1. Architectural simplicity.
2. Two front-ends that share one core, so neither is a port of the other.
3. No cost over calling liburing directly, beyond what a front-end needs.

When goals conflict, the higher one wins.

**You pay only for what you use.** A program that uses only coroutines compiles
no sender code, and the reverse. Features such as cancellation, linked
timeouts and registered buffers are opted into, and a program that doesn't use
them runs no slower.

## How we work

iopia is built in steps, one layer at a time, and each layer lands with its
tests before the next one starts. The roadmap below lists the steps.

We use liburing for the system calls and the ring's memory layout. The layers
above it (the ring wrapper, the completion protocol, the event loop and both
front-ends) are ours, because their design is the point of the project.

## Conventions

iopia follows the conventions in simon's
[framework/Design.md](https://github.com/sempuki/simon/blob/main/framework/Design.md#conventions):
`Out`, `InOut` and `Depend` at call sites, trailing return types, `maybe_` for
nullable getters, one `public:`, `protected:` and `private:` section per class,
and the rest.

**Code lives in the `iopia` package and the `iopia` namespace.** Headers are
included from the repository root (`#include "iopia/ring.hpp"`), and
implementation details live in `iopia::internal`.

**Errors are `std::expected<T, lib::Status>`.** liburing reports failure as a
negative `errno`. iopia maps it to a status domain, an `enum class` with its
messages in a `lib::EnumStatusKindConditionMixin` specialization, as simon's
`format/format_error.hpp` does. The value a completion carries (`cqe->res`) is
a result of the operation, so a front-end passes it on as the operation's
outcome.

**Types that can fail to construct have a factory.** A constructor can't return
an error, so `Ring::make(...)` returns `std::expected<Ring, lib::Status>`.

**Contract checks use lib's `CHECK_PRECONDITION`, `CHECK_POSTCONDITION`,
`CHECK_INVARIANT` and `CHECK_UNREACHABLE`.**

**Tests are Catch2, through `@lib//base:testing`.** One `TEST_CASE` per type,
`SECTION("ShouldXGivenY")` inside it, and each section split into
`// Preconditions.`, `// Under Test.` and `// Postconditions.`

## Architecture

```
   coroutines             senders and receivers
  (Task<T>, co_await)    (scheduler, sender, operation state)
           \                     /
            \                   /
          completion protocol (user_data -> operation)
                      |
               ring and event loop
                      |
                   liburing
```

### Ring

`Ring` owns one `io_uring`. It is move-only, made by `Ring::make`, and calls
`io_uring_queue_exit` when destroyed unless it was moved from.

### Completion protocol

Each submission's `user_data` points at an operation that outlives it. When
the completion arrives, the event loop calls the operation with the
completion's `res` and `flags`. The two front-ends differ only in what an
operation does then. A coroutine's operation resumes its `coroutine_handle`,
and a sender's operation state calls its receiver.

### Event loop

The loop submits what is queued, reaps completions and dispatches each one.
It decides when to wait in the kernel, what happens when the submission queue
is full, and when `run()` returns.

### Coroutine front-end

`Task<T>` is a lazy coroutine type, and each io_uring operation is an
awaitable. The awaitable prepares its submission in `await_suspend`, when the
coroutine's handle is known, and the completion resumes the coroutine.

### Sender front-end

A scheduler's `schedule()` returns a sender, and each io_uring operation is a
sender. The operation state that `connect` returns is the completion
protocol's operation, so it lives exactly as long as the submission does.

### Threading

A ring belongs to one thread. lib's `Status` keeps its messages in a buffer
that isn't thread-safe, which is one more reason to keep a ring and its
completions on one thread until a later step decides otherwise.

## Decisions

| Decision | Why |
|---|---|
| The namespace and package are `iopia`. | One name for the project, the code and its includes. |
| liburing 2.15 comes from the Bazel Central Registry. | Every dependency comes from the registry, as in simon. The registry's build reads the host's kernel headers once; see `3rd_party/README.md`. |
| C++26, from lib's `COPTS`. | Shared with simon and volcano. Coroutines need only C++20. |
| A ring belongs to one thread. | Simplest to start with, and `lib::Status` isn't thread-safe. |

## Open questions

- **Own sender and receiver concepts, or stdexec?** Our own teach more and keep
  the dependency list short. stdexec gives its algorithms (`then`,
  `when_all`, and so on) for free. GCC 16's standard library has no
  `std::execution` yet. Decide before step 5.
- **How the loop calls an operation:** a function pointer stored in the
  operation, or a virtual function. A function pointer keeps operations
  standard-layout and avoids a vtable.
- **What happens when the submission queue is full:** submit and retry, or
  report an error to the caller.
- **Cancellation:** how a coroutine and a stop token each map to
  `IORING_OP_ASYNC_CANCEL`.

## Roadmap

1. **Ring.** `Ring::make`, move-only ownership, and a test that submits a NOP
   and reaps its completion.
2. **Completion protocol.** The operation type, `user_data` dispatch, and
   `run_once()`.
3. **Event loop.** Batching, a full submission queue, and when `run()` returns.
4. **Coroutine front-end.** `Task<T>`, and awaitables for nop, read, write and
   timeout.
5. **Sender front-end.** A scheduler and the same operations as senders.
6. **Bridges.** Awaiting a sender inside a `Task`, and a `Task` as a sender.
7. **Further features.** Cancellation, linked timeouts, multishot accept and
   receive, registered buffers, and waking a loop from another thread.
