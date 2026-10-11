# Using iopia

This guide shows how to use iopia's pieces. [README.md](README.md) covers
building it, and [Design.md](Design.md) explains why it works the way it does.

## Choosing a layer

| Layer | Header | Use it for |
|---|---|---|
| Coroutines | `iopia/task.hpp`, `iopia/awaitable.hpp` | Most code. Each operation reads as a `co_await`. |
| Handlers on a loop | `iopia/loop.hpp`, `iopia/operation.hpp` | Code that wants a callback per completion and no coroutine frames. |
| The ring | `iopia/ring.hpp` | Direct use of one `io_uring`, with no dispatch. |

Senders and receivers arrive in roadmap step 5.

Every layer runs on one thread. A ring and the loop that owns it belong to the
thread that made them.

## Errors

A function that can fail returns `std::expected<T, lib::Status>`. liburing's
negative `errno` values become one of these `IoError` conditions.

| Condition | errno |
|---|---|
| `UNSUPPORTED` | `ENOSYS`, `EOPNOTSUPP` |
| `DENIED` | `EPERM`, `EACCES` |
| `EXHAUSTED` | `ENOMEM`, `EMFILE`, `ENFILE` |
| `BUSY` | `EAGAIN`, `EBUSY` |
| `INTERRUPTED` | `EINTR` |
| `INVALID` | `EINVAL`, `EFAULT`, `EBADF` |
| `OTHER` | anything else |

Compare a status with a condition through `lib::watch`. Its message names the
call and the errno.

```cpp
if (!result && result.error() == lib::watch(IoError::BUSY)) {
  // Try again later.
}
std::println("{}", result.error().message());  // "read: Bad file descriptor (errno 9)"
```

## Coroutines

### Writing a task

A coroutine that returns `Task<T>` is a task. It is lazy, so calling it only
creates it, and it starts when something awaits it or runs it. A task is
move-only.

```cpp
auto read_header(Depend<Loop> loop, std::int32_t file,
                 std::span<std::byte> header)
    -> Task<std::expected<std::uint32_t, lib::Status>> {
  co_return co_await Read{loop, file, header, 0};
}

auto header_size(Depend<Loop> loop, std::int32_t file) -> Task<std::uint32_t> {
  std::array<std::byte, 64> header;
  auto read = co_await read_header(loop, file, header);
  co_return read ? *read : 0;
}
```

A task can await another task, and resumes with the other task's value when it
finishes.

### Running a task

`run(InOut(loop), task)` starts the task and runs the loop until the task
finishes. It returns the task's value, or the loop's error if waiting failed
for a reason other than a signal.

```cpp
auto ring = Ring::make(64);
if (!ring) { /* ring.error() */ }
Loop loop{*std::move(ring)};

std::expected<std::uint32_t, lib::Status> size =
    run(InOut(loop), header_size(Depend(loop), file));
```

A task whose value is itself a `std::expected` comes back nested, as
`std::expected<std::expected<T, lib::Status>, lib::Status>`. The outer one
reports the loop, and the inner one the operation.

### Awaitables

Each awaitable is one io_uring operation. Construct it in the `co_await`.

| Awaitable | Result |
|---|---|
| `NoOp{loop}` | `std::expected<void, lib::Status>` |
| `Read{loop, file, buffer, offset}` | `std::expected<std::uint32_t, lib::Status>`, the bytes read |
| `Write{loop, file, buffer, offset}` | `std::expected<std::uint32_t, lib::Status>`, the bytes written |
| `Timeout{loop, duration}` | `std::expected<void, lib::Status>`, once `duration` has passed |

An offset of -1 reads or writes at the file's current position. A buffer must
stay valid until the `co_await` returns.

If the loop has no submission queue entry to hand out, the awaitable doesn't
suspend, and it reports `IoError::BUSY`.

### Rules

- **A task can't throw.** An exception that escapes a task terminates the
  program.
- **Don't destroy a task while it waits on an operation.** The kernel holds
  the address of the awaitable in the task's frame. Run the task to the end.
  This rule lasts until cancellation exists (roadmap step 7).

## Handlers on a loop

### Making a loop

A `Loop` owns a `Ring`. It can be neither copied nor moved.

```cpp
auto ring = Ring::make(64);
if (!ring) { /* ring.error() */ }
Loop loop{*std::move(ring)};
```

### Submitting

A handler is a `noexcept` callable that takes a `Completion`. It becomes an
operation in one of two ways.

**You own it.** Put a `HandlerOperation` wherever you like, take an entry for
it, and prepare the entry with an `io_uring_prep_*` function. Keep the
operation alive until its last completion. It can't be moved.

```cpp
HandlerOperation operation{[&](Completion completion) noexcept {
  // completion.result is the operation's result, or a negative errno.
}};
if (::io_uring_sqe* submission = loop.maybe_submission(Depend(operation))) {
  ::io_uring_prep_nop(submission);
}
```

**The loop owns it.** `queue(prepare, handler)` takes a `noexcept` function
that prepares the entry, and the handler. The loop keeps the handler until its
last completion.

```cpp
bool queued = loop.queue(
    [](::io_uring_sqe* submission) noexcept { ::io_uring_prep_nop(submission); },
    [](Completion completion) noexcept { /* ... */ });
```

Prefer your own `HandlerOperation` when you can keep it alive, since it costs
nothing per completion. The loop-owned kind frees you from lifetimes, and
allocates once a capture is larger than 24 bytes. Design.md's Handlers section
compares the two.

When the submission queue is full, both submit what the queue holds and try
once more. A null entry from `maybe_submission`, or `false` from `queue`,
means that failed too, so try again after the loop has run.

### Running

| Call | What it does |
|---|---|
| `loop.run_once()` | Submits what is prepared, waits for at least one completion, calls every ready handler, and returns how many it called. Returns 0 at once if nothing is pending. |
| `loop.run()` | Calls `run_once()` until nothing is pending, or until a handler calls `loop.stop()`. Retries a wait a signal interrupted. |
| `loop.stop()` | Makes the `run()` in progress return after the completions it is handling. The next `run()` starts fresh. |

A handler may submit more operations. They go out on the next `run_once()`.

### Rules

- **Handlers and prepare functions are `noexcept`.** The compiler rejects
  ones that aren't. A handler that fails reports it through state its owner
  reads.
- **A multishot operation calls its handler several times.** Every
  completion but the last has `IORING_CQE_F_MORE` set in `completion.flags`.
- **Destroying a loop cancels what is pending.** Its destructor cancels every
  operation and calls each handler one last time, with `-ECANCELED`. Declare
  whatever a pending handler or operation uses before the loop, so it
  outlives the loop.

```cpp
std::int32_t calls = 0;                 // Outlives the loop.
Loop loop{*std::move(ring)};
```

### Writing an operation type

A front-end, or any code that wants its own operation, derives from
`Operation` and sets `complete` to a `noexcept` function that casts back to
its own type.

```cpp
struct CountingOperation final : Operation {
  CountingOperation() {
    complete = [](Operation& operation, Completion) noexcept {
      ++static_cast<CountingOperation&>(operation).calls;
    };
  }
  std::int32_t calls = 0;
};
```

## The ring

`Ring` gives direct access to one `io_uring`, with no dispatch.

| Call | What it does |
|---|---|
| `Ring::make(entries, flags)` | Sets up a ring with at least `entries` entries. `flags` are liburing's `IORING_SETUP_*` bits. |
| `ring.maybe_submission()` | The next free entry, or null if the queue is full. |
| `ring.submit()` | Hands every prepared entry to the kernel, and returns how many it took. |
| `ring.submit_and_wait(count)` | Submits, then waits until at least `count` completions are ready, in one system call. |
| `ring.wait()` | Waits for the next completion and returns a copy of it. |
| `ring.maybe_completion()` | A copy of the next ready completion, or none. Never waits. |
| `ring.take_completions(Out(batch))` | Copies as many ready completions as fit in the `std::array` `batch`, and returns how many. Never waits. |

Completions come out as `Completion` copies, holding `user_data`, `flags` and
`result`, so you never hold a pointer into the completion queue.
