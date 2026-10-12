# iopia design

iopia gives Linux's io_uring two C++ front-ends, coroutines and senders and
receivers, both modeled on the C++ standard's designs and built on one ring
and one completion protocol. On top of them, iopia builds a gateway that moves
messages between local publish-subscribe systems, such as iceoryx2, and the
network.

This document records the architecture, the decisions behind it, and the
roadmap. Update it when a decision changes. How to use iopia is in
[Using.md](Using.md), and how io_uring maps onto coroutines is in
[Design.coroutine.md](Design.coroutine.md).

## Goals

The library comes first, in priority order:

1. Architectural simplicity.
2. Two front-ends that share one core, so neither is a port of the other.
3. No cost over calling liburing directly, beyond what a front-end needs.
4. Front-ends modeled on the standard's designs, C++20 coroutines and C++26's
   `std::execution`, so building them teaches how the standard works.

The gateway is the library's first real user:

5. A gateway that adds as little latency and as few copies as each transport
   allows, and runs a whole data path on one thread's ring.

When goals conflict, the higher one wins. The gateway doesn't bend the
library; if it needs something the library lacks, the library gains it as a
general feature.

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

**iopia is a way to learn the standard.** We write our own task type, sender
and receiver concepts, and algorithms instead of taking them from a library
such as stdexec. They follow the standard's names and shapes where that
teaches something, and depart from it where that keeps iopia simpler. Strict
conformance isn't a goal. If writing a piece ourselves stops teaching
anything, we can revisit taking it from a library.

The gateway uses iceoryx2 through its C++ binding. It starts after the
library's front-ends are done (roadmap step 8).

## Conventions

iopia follows the conventions in simon's
[framework/Design.md](https://github.com/sempuki/simon/blob/main/framework/Design.md#conventions):
`Out`, `InOut` and `Depend` at call sites, trailing return types, `maybe_` for
nullable getters, one `public:`, `protected:` and `private:` section per class,
and the rest.

**Documents live beside their code.** A package's `Design.md`, any
`Design.<topic>.md`, and its `Using.md` sit in the package's directory.

**Code lives in the `iopia` package and the `iopia` namespace.** Headers are
included from the repository root (`#include "iopia/ring.hpp"`), and
implementation details live in `iopia::internal`. The gateway lives in the
`gateway` package and the `iopia::gateway` namespace, so the library never
depends on iceoryx2.

**C symbols are written with the global namespace qualifier,**
`::io_uring_get_sqe(...)` and `::io_uring_sqe*`, so a reader can tell
liburing's C API from iopia's own names. Macros such as `IORING_SETUP_SQPOLL`
and `EINVAL` can't be qualified and stay as they are. A function-like macro,
such as `io_uring_for_each_cqe`, gets a comment saying it's a macro, so the
missing `::` doesn't look like a mistake.

**`Out`, `InOut` and `Depend` are written without `lib::`.** They are lib's
parameter vocabulary, and the project has only one of each.
`iopia/argument.hpp` declares them in the `iopia` namespace, as simon's
`core/argument.hpp` does.

**Integers have fixed widths.** iopia writes `std::uint32_t` where C would
write `unsigned`. Sizes and
indices into containers stay `std::size_t`. liburing's `unsigned` parameters
take a `std::uint32_t` without a cast, since the two are the same type on
Linux. A value a C call returns, such as liburing's `int` results, keeps its
type only until iopia converts it.

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
                  gateway (package gateway)
           iceoryx2 ports <-> network sockets
           /                              \
   coroutines                    std::execution
  (Task<T>, co_await)    (scheduler, sender, operation state)
           \                     /
            \                   /
          completion protocol (user_data -> operation)
                      |
               ring and event loop
                      |
                   liburing
```

The gateway may use either front-end. It is written against their public
interfaces, as any user's code would be.

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

**Completions leave the ring as copies.** No caller holds a pointer into the
completion queue, so a completion queue entry (CQE) can go back to the kernel
before anyone handles it. `take_completions` copies a batch and returns all
its CQEs with one
`io_uring_cq_advance`. It reads the queue with `io_uring_for_each_cqe` and
never enters the kernel.

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
derives from `Operation`, sets `complete` to a `noexcept` function that casts
the `Operation&` back to its own type, and adds whatever state it needs.

Using.md shows an operation type.

#### Handlers

A handler is a callable that the loop calls with each `Completion`. It must
be `noexcept`, which the `Handler` concept checks
(`std::is_nothrow_invocable_v<H&, Completion>`). A handler becomes an
operation in one of two ways, which differ in who owns it while the kernel
holds its address.

**The caller owns it.** `HandlerOperation<H>` stores the handler `H` by value
and calls it with each `Completion`. It lives wherever the caller puts it (on
the stack, in a member, in a coroutine frame or a sender's operation state),
and the caller keeps it alive until its last completion. It allocates nothing,
takes handlers of any size, and makes one indirect call per completion. It
can be neither copied nor moved, so it can't leave the address the kernel
holds. Both front-ends build on this kind.

**The loop owns it.** `loop.queue(prepare, handler)` takes a function that
prepares the submission queue entry (SQE) and a handler to call with each
completion. The loop stores the handler in a node of its own, a
`HandlerNode` holding a `std::move_only_function<void(Completion) noexcept>`,
and returns the node to a free list after the last completion. The caller has
nothing to keep alive.

| | Caller-owned | Loop-owned |
|---|---|---|
| Who keeps it alive | the caller | the loop |
| Allocation | none | none once the free list is warm, for captures of 24 bytes or less |
| Handler size | any | any; larger captures allocate inside `std::move_only_function` |
| Indirect calls per completion | one | two (`complete`, then the `std::move_only_function`) |
| Per-operation storage | the handler | the node: `Operation` plus a 40-byte `std::move_only_function` |

The 24 and 40 bytes are libstdc++'s, measured with GCC 16 and Clang 22.
`std::move_only_function` gives the loop-owned kind its small-buffer storage,
moves and destruction, so iopia doesn't write them.

**Handlers can't throw, and neither can prepare functions.** The compiler
enforces it. `complete` is a `noexcept` function pointer, `HandlerOperation`
and `queue` accept only handlers the `Handler` concept allows, and `queue`
accepts only prepare functions the `Preparer` concept allows
(`std::is_nothrow_invocable_v<P, ::io_uring_sqe*>`). A prepare function runs
after the loop has handed out the SQE and counted the operation as pending,
so a throw there would leave a half-prepared SQE behind. A handler that fails
reports it through its own state, such as an error it stores for its owner. If an exception escapes
anyway, the program terminates.

The loop can therefore dispatch completions without ever stopping partway
through a batch, and needs no way to resume one. A loop-owned node goes back
to the free list before its handler's last call, by moving the handler out
first, so the handler can queue a new operation that reuses its node.

### Event loop

`Loop` (`iopia/loop.hpp`) owns a `Ring` and counts the operations it has
handed SQEs to whose last completion hasn't arrived. It
can be neither copied nor moved, because its loop-owned nodes and the
operations that resubmit through it hold its address.

Using.md lists its calls. Three of their behaviors are decisions.

- **A full submission queue is submitted and retried once.** Most programs
  never see a full queue, and a caller has one rule to follow: a null SQE,
  or `false` from `queue`, means try again later.
- **`run()` returns when nothing is pending or a handler calls `stop()`.** It
  retries a wait a signal interrupted (`-EINTR`), and returns any other error.
  `stop()` affects only the `run()` in progress.
- **A completion whose `user_data` is 0 has no operation,** and fails a
  contract check.

**Destroying a loop cancels and drains what is pending.** Tearing down the
ring with operations pending would mean their completions are never reaped,
so loop-owned handlers would never be destroyed. The destructor submits one
`io_uring_prep_cancel` with `IORING_ASYNC_CANCEL_ANY` (Linux 5.19) and runs
until nothing is pending, so every operation sees its last completion, the
cancelled ones with `-ECANCELED`. A caller-owned operation that is still
pending must therefore outlive the loop.

**Completions are reaped in batches.** `run_once()` copies up to 32 ready
completions into an array on its stack with `take_completions`, which returns
their CQEs to the kernel with one store to the queue's head, then
dispatches the copies. It repeats while a batch comes back full.

```
take_completions(batch)   one read of the tail, copy up to 32
io_uring_cq_advance(n)    one store to the head; the slots are free again
dispatch(batch[0..n))     handlers run on the copies
```

Batching replaces a store to the shared head per completion with one per
batch, which keeps the cache line the kernel reads from moving between cores
on every completion. Returning the slots before dispatching keeps them free
for completions the kernel posts while handlers run. Handlers can't throw, so
a batch is always dispatched to the end, and the loop never has to keep one
to finish later. Stopping after a batch that isn't full means a stream of
completions that keeps arriving can't hold `run_once()` forever.

### Coroutine front-end

`Task<T>` (`iopia/task.hpp`) is a lazy coroutine, and each io_uring operation
is an awaitable (`iopia/awaitable.hpp`). Using.md shows how to write and run
tasks. [Design.coroutine.md](Design.coroutine.md) explains how io_uring's
concepts map onto C++ coroutines, and the choices behind this section.

**An awaitable is the completion protocol's operation.** Each derives from
`Operation` through `internal::Awaitable`, so it lives in the awaiting
coroutine's frame, at a fixed address, for as long as the kernel holds it.
`await_suspend` takes an SQE from the loop and prepares it. The completion
stores the result and resumes the coroutine, from inside the loop's dispatch.
`await_resume` turns a negative result into a `lib::Status` through
`raise_io_error`. A timeout that expires completes with `-ETIME`, which
`Timeout` treats as success.

If the loop can't hand out an SQE, `await_suspend` returns `false`, the
coroutine continues without suspending, and the awaitable reports `-EBUSY`
as `IoError::BUSY`.

**A task resumes its awaiter in one of two ways.** Starting a child task
resumes it from the parent's `await_suspend`.

| The child | What happens |
|---|---|
| finishes before it suspends | Its final suspend returns to the parent's `await_suspend`, which returns `false`, and the parent continues. |
| suspends on an operation, and finishes later | Its final suspend transfers to the parent (symmetric transfer). |

The first case keeps a loop over children that never suspend from growing the
stack. Symmetric transfer alone would do that only where the compiler makes
it a tail call, and GCC 16 doesn't at `-O0`, which is Bazel's default build.
A test of a million such children overflowed the stack there.

**`run()` drives one task to completion.** It resumes the task, then calls
`loop.run_once()` until the task is done, retrying an interrupted wait and
returning any other error. A task that waits on nothing the loop knows about
would never finish, so `run()` checks that something is pending each time.

**Tasks don't throw.** `unhandled_exception` calls `std::terminate`, as a
handler's escaping exception does.

**A task suspended on an operation must not be destroyed.** The kernel holds
the address of the awaitable in its frame. Until cancellation (step 7) can
cancel the operation first, a task that is suspended must be run to the end.

The coroutine front-end uses only `<coroutine>`, so a program that uses it
compiles no sender code.

### Sender front-end

The sender front-end is iopia's own implementation of C++26's
`std::execution` design. iopia defines its own `scheduler`, `sender`,
`receiver` and `operation_state` concepts in the `iopia` namespace, after the
standard's. A loop's scheduler has a `schedule()` that returns a sender, and
each io_uring operation is a sender too. The operation state that `connect`
returns is the completion protocol's operation, so it lives exactly as long
as the submission does.

iopia writes the algorithms it needs (`then`, `when_all`, `let_value`,
`sync_wait` and so on) one at a time, as the roadmap reaches them.
Completions map onto the three channels as follows.

| Completion | Channel |
|---|---|
| `res >= 0` | `set_value` with the result |
| `res == -ECANCELED` after a stop request | `set_stopped` |
| any other `res < 0` | `set_error` with the `lib::Status` |

### Threading

A ring belongs to one thread. lib's `Status` keeps its messages in a buffer
that isn't thread-safe, which is one more reason to keep a ring and its
completions on one thread until a later step decides otherwise.

### Gateway

This section moves to `gateway/Design.md` when the `gateway` package exists.

The gateway moves messages along three kinds of path.

| Path | From | To |
|---|---|---|
| Network to local | a socket | an iceoryx2 publisher |
| Local to network | an iceoryx2 subscriber | a socket |
| Local to local | one local system | another, e.g. iceoryx2 and ROS 2 |

The first version connects iceoryx2 with the network. DDS, and with it ROS 2
on its DDS middlewares, waits for a later decision (see Open questions). ROS 2
nodes that use `rmw_iceoryx2` already speak iceoryx2, so the gateway reaches
them without DDS.

**Each path runs on one thread and one ring.** The gateway scales by giving
each thread its own set of topics, so no message crosses threads on its way
through. This follows "a ring belongs to one thread", and needs no locks on
the data path.

**iceoryx2 wakes the ring through a file descriptor.** An iceoryx2 `Listener`
exposes the file descriptor it waits on. The gateway arms a multishot
`IORING_OP_POLL_ADD` on it, and when it fires, takes the listener's events with
`try_wait` and drains the matching subscribers without blocking. iceoryx2's
data stays in shared memory and never passes through the kernel; only the
wakeup does.

**Network to local copies once.** The gateway receives into kernel-chosen
buffers, either zero-copy receive (zcrx, `IORING_OP_RECV_ZC`) where the NIC
supports it, or provided buffer rings otherwise. It then copies each payload
into a sample loaned from the iceoryx2 publisher, and returns the receive
buffer at once.

The receive buffer and the iceoryx2 sample can't be one piece of memory,
because the kernel chooses which zcrx buffer it fills, and iceoryx2 chooses
which sample it loans. Publishing offsets into the zcrx area instead would let
the slowest subscriber hold receive buffers, and once the refill queue runs
dry the kernel falls back to copying anyway. One copy keeps the NIC's queue
healthy and makes every local subscriber zero-copy.

**Local to network sends from the sample.** The gateway sends straight from
the iceoryx2 sample with `IORING_OP_SEND_ZC`, and keeps the sample until the
send's notification completion says the kernel is done with its memory. If
iceoryx2's shared memory segment can be registered as a fixed buffer, the
send uses it (Linux 6.0 for `SEND_ZC`; Linux 7.2 for registered buffers on
plain `SEND`).

**The library supplies the operations the gateway needs** as general
features: multishot poll, receive with provided buffers, zero-copy receive and
send, and registered buffers. The gateway adds only iceoryx2 ports and the
network protocol between gateways.

## Decisions

| Decision | Why |
|---|---|
| The namespace and package are `iopia`. | One name for the project, the code and its includes. |
| liburing 2.15 comes from the Bazel Central Registry. | Every dependency comes from the registry, as in simon. The registry's build reads the host's kernel headers once; see `3rd_party/README.md`. |
| C++26, from lib's `COPTS`. | Shared with simon and volcano. Coroutines need only C++20. |
| A ring belongs to one thread. | Simplest to start with, and `lib::Status` isn't thread-safe. |
| The loop calls an operation through a function pointer stored in it. | Operations carry no vtable, and each front-end supplies its own function. A virtual function would make every operation polymorphic. |
| Handlers are caller-owned (`HandlerOperation<H>`) or loop-owned (`loop.queue`, on `std::move_only_function`). | Caller-owned costs nothing and is what the front-ends need. Loop-owned frees the caller from lifetimes, and reuses the standard library's small-buffer storage instead of a pool of hand-made slots. |
| A loop cancels and drains pending operations when destroyed. | Every handler sees its last completion and is destroyed; nothing leaks with the ring. |
| The loop reaps completions in batches of 32, copied out before dispatch. | One store to the shared head per batch instead of per completion, and the slots are free again before handlers run. |
| A full submission queue is submitted and retried once; null means it still failed. | Most programs never see a full queue, and callers keep one simple contract: null means try later. |
| `run()` returns when nothing is pending or a handler calls `stop()`. | It is what `sync_wait` and a coroutine's top-level run will need, without a timeout API yet. |
| `Task<T>` is iopia's own coroutine type and doesn't await senders. | Coroutine users compile no sender code, per "you pay only for what you use". This settles whether to follow `std::execution::task`; step 6 bridges the two front-ends. |
| A child task that finishes before suspending returns to its parent's `await_suspend`. | Symmetric transfer isn't a tail call under GCC at `-O0`, and a loop over such children would overflow the stack. |
| Handlers and prepare functions are `noexcept`, checked at compile time. | The loop never has to recover from a dispatch that stopped partway, which keeps batched reaping simple. We may revisit this later. |
| iopia writes its own coroutine task, sender concepts and algorithms, modeled on the standard's. | Building them is how we learn the standard, which matters more here than conformance or a shorter list of things to write. This settles the earlier question of our own concepts or stdexec's. |
| The gateway is a separate package, `gateway`, on the library's public interfaces. | The library never depends on iceoryx2, and the gateway tests the interfaces the way any user would. |
| The gateway's first version connects iceoryx2 and the network; DDS comes later. | iceoryx2 exposes a file descriptor and shared memory, which fit io_uring directly. DDS brings a choice between writing RTPS and linking a library with threads of its own. |
| Network to local copies each payload once, into an iceoryx2 sample. | The kernel picks the receive buffer and iceoryx2 picks the sample, so they can't be one. Returning receive buffers at once keeps the NIC from falling back to copying. |

## Open questions

- **The network protocol between gateways:** a framing of iopia's own over TCP
  or UDP, or an existing one such as Zenoh's or RTPS. Decide before step 9.
- **How the gateway speaks DDS:** implement RTPS on io_uring so the ring owns
  every socket, or link a DDS library such as Cyclone DDS and hand messages
  between its threads and the ring. Local-to-local paths to DDS wait on this.
- **Registering iceoryx2's shared memory as a fixed buffer:** whether iceoryx2
  exposes its segments' addresses and sizes, and whether it remaps them while
  the gateway runs.
- **Destroying a suspended task:** whether destroying a task that is
  suspended on an operation should cancel the operation and wait for its
  completion, once cancellation exists.
- **Cancellation:** how a coroutine and a stop token each map to
  `IORING_OP_ASYNC_CANCEL`.

## Roadmap

1. **Ring.** `Ring::make`, move-only ownership, and a test that submits a
   no-op and reaps its completion.
2. **Completion protocol.** The operation type, `user_data` dispatch, and
   `run_once()`.
3. **Event loop.** Batched reaping, a full submission queue, and `run()` with
   `stop()`.
4. **Coroutine front-end.** `Task<T>`, and awaitables for no-op, read, write
   and timeout.
5. **Sender front-end.** iopia's sender concepts, a scheduler, the same
   operations as senders, and the first algorithms (`then`, `sync_wait`).
6. **Bridges.** Awaiting a sender inside a `Task`, and a `Task` as a sender.
7. **Further features.** Cancellation, linked timeouts, multishot accept,
   receive and poll, provided buffer rings, registered buffers, zero-copy send
   and receive, and waking a loop from another thread.
8. **iceoryx2 ports.** Awaiting an iceoryx2 listener through multishot poll,
   draining subscribers, and publishing loaned samples, in the `gateway`
   package.
9. **Network paths.** Network to local and local to network between two
   gateways, with the protocol chosen in Open questions.
10. **Local-to-local paths.** Bridging to ROS 2 and DDS, once the DDS question
    is decided.
