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

**C symbols are written with the global namespace qualifier,**
`::io_uring_get_sqe(...)` and `::io_uring_sqe*`, so a reader can tell
liburing's C API from iopia's own names. Macros such as `IORING_SETUP_SQPOLL`
and `EINVAL` can't be qualified and stay as they are.

**Names avoid abbreviations,** except where they come from the C API:
`submission`, not `sqe`. `IoError` keeps "io", because it follows io_uring's
naming.

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

**The `io_uring` lives on the heap,** held by a `std::unique_ptr` whose deleter
calls `io_uring_queue_exit`. A moved-from `unique_ptr` is null and runs no
deleter, so `Ring`'s moves are the defaults, and there is no flag to say
whether a `Ring` still owns its `io_uring`. The `io_uring` also keeps its
address when the `Ring` moves, though nothing relies on that yet.

It could live in the `Ring` by value instead. liburing's `struct io_uring`
holds only an fd, sizes and flags, and pointers into the kernel's mapped rings,
none into itself, and the kernel never learns its address. So it can be copied
to a new address safely.

| | Heap (chosen) | By value |
|---|---|---|
| Allocations per ring | one | none |
| Each `get_sqe`, `submit` and `wait` | loads the pointer first | reaches the struct directly |
| Move code | the defaults | hand-written, about 10 lines |
| Moved-from marker | a null pointer | `ring_fd = -1`, or a separate flag |
| Size of `Ring` | 8 bytes | about 216 bytes |

We keep the heap for the simpler move. Goal 3 argues for the value instead, so
measure the hot calls before deciding to change it.

`maybe_submission()` returns the next free submission queue entry, or null when
the queue is full. `wait()` copies a completion into a `Completion` and returns
the entry to the kernel at once, so no caller holds a pointer into the
completion queue.

### Completion protocol

Each submission's `user_data` points at an operation. When a completion
arrives, the event loop calls the operation with the completion's `res` and
`flags`, through a function pointer the operation stores. The two front-ends
differ only in what an operation does then. A coroutine's operation resumes
its `coroutine_handle`, and a sender's operation state calls its receiver.

**An operation must stay alive, at the same address, until its last
completion has been dispatched.** That is later than its submission. Most
operations complete once. A multishot operation (multishot accept, receive or
timeout) completes several times, and every completion but the last has
`IORING_CQE_F_MORE` set in its flags.

`Operation` (`iopia/operation.hpp`) holds only that function pointer,
`complete`, which takes the operation and its `Completion`. An operation type
derives from `Operation`, sets `complete` to a function that casts the
`Operation&` back to its own type, and adds whatever state it needs.

```cpp
struct CountingOperation final : Operation {
  CountingOperation() {
    complete = [](Operation& operation, Completion) {
      ++static_cast<CountingOperation&>(operation).calls;
    };
  }
  int calls = 0;
};
```

#### Callables

A lambda or other callable becomes an operation in one of two ways, which
differ in who owns it while the kernel holds its address.

**The caller owns it.** `CallableOperation<F>` stores the callable `F` by
value and calls it with each `Completion`. It lives wherever the caller puts
it (on the stack, in a member, in a coroutine frame or a sender's operation
state), and the caller keeps it alive until its last completion. It allocates
nothing, takes callables of any size, and makes one indirect call per
completion. It can be neither copied nor moved, so it can't leave the address
the kernel holds. Both front-ends build on this kind.

```cpp
CallableOperation operation{[&](Completion completion) { ... }};
::io_uring_prep_nop(loop.maybe_submission(lib::Depend(operation)));
```

**The loop owns it.** `loop.queue(prepare, callback)` takes a function that
prepares the submission queue entry and a callable to call with each
completion. The loop stores the callable in a node of its own, a
`CallbackOperation` holding a `std::move_only_function<void(Completion)>`, and
returns the node to a free list after the last completion. The caller has
nothing to keep alive.

```cpp
bool queued = loop.queue(
    [](::io_uring_sqe* submission) { ::io_uring_prep_nop(submission); },
    [](Completion completion) { ... });
```

| | Caller-owned | Loop-owned |
|---|---|---|
| Who keeps it alive | the caller | the loop |
| Allocation | none | none once the free list is warm, for captures of 24 bytes or less |
| Callable size | any | any; larger captures allocate inside `std::move_only_function` |
| Indirect calls per completion | one | two (`complete`, then the `std::move_only_function`) |
| Per-operation storage | the callable | the node: `Operation` plus a 40-byte `std::move_only_function` |

The 24 and 40 bytes are libstdc++'s, measured with GCC 16 and Clang 22.
`std::move_only_function` gives the loop-owned kind its small-buffer storage,
moves and destruction, so iopia doesn't write them.

**Exceptions from a callback propagate out of `run_once()`.** The entry was
already returned to the kernel, and the completions still waiting stay in the
completion queue for the next `run_once()`. A loop-owned node goes back to the
free list before its last callback runs, by moving the callable out first, so
a throw loses nothing. That also lets a callback queue a new operation that
reuses its node.

### Event loop

`Loop` (`iopia/loop.hpp`) owns a `Ring` and counts the operations it has
handed submission queue entries to whose last completion hasn't arrived. It
can be neither copied nor moved, because its loop-owned nodes and the
operations that resubmit through it hold its address.

- `maybe_submission(Depend(operation))` returns the next free submission
  queue entry with its `user_data` set to the operation's address, or null
  when the queue is full. `Depend` marks that the loop keeps a reference that
  can dangle.
- `queue(prepare, callback)` does the same for a loop-owned callable, and
  returns false when the queue is full.
- `run_once()` submits what is prepared and waits for at least one completion,
  in one system call (`io_uring_submit_and_wait`). Then it calls back the
  operation of every completion that is ready, and returns how many it called.
  With nothing pending it returns 0 without waiting. A callback may submit
  more operations; they go out on the next `run_once()`.
- A completion whose `user_data` is 0 has no operation, and fails a contract
  check.

**Destroying a loop cancels and drains what is pending.** Tearing down the
ring with operations pending would mean their completions are never reaped,
so loop-owned callables would never be destroyed. The destructor submits one
`io_uring_prep_cancel` with `IORING_ASYNC_CANCEL_ANY` (Linux 5.19) and runs
until nothing is pending, so every operation sees its last completion, the
cancelled ones with `-ECANCELED`. A caller-owned operation that is still
pending must therefore outlive the loop. A callback that throws during the
drain terminates the program, since destructors don't throw.

Step 3 decides the rest: when `run()` returns, what happens when the
submission queue is full, and whether to batch reaping with
`io_uring_peek_batch_cqe` and a single `io_uring_cq_advance`. Today each
completion is copied and returned to the kernel one at a time.

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
| The loop calls an operation through a function pointer stored in it. | Operations carry no vtable, and each front-end supplies its own function. A virtual function would make every operation polymorphic. |
| Callables are caller-owned (`CallableOperation<F>`) or loop-owned (`loop.queue`, on `std::move_only_function`). | Caller-owned costs nothing and is what the front-ends need. Loop-owned frees the caller from lifetimes, and reuses the standard library's small-buffer storage instead of a pool of hand-made slots. |
| A loop cancels and drains pending operations when destroyed. | Every callable sees its last completion and is destroyed; nothing leaks with the ring. |

## Open questions

- **Own sender and receiver concepts, or stdexec?** Our own teach more and keep
  the dependency list short. stdexec gives its algorithms (`then`,
  `when_all`, and so on) for free. GCC 16's standard library has no
  `std::execution` yet. Decide before step 5.
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
