# io_uring and C++ coroutines

This document explains how io_uring's concepts map onto C++ coroutines, for a
reader who knows io_uring better than coroutines. It covers the coroutine
machinery first, then the mapping iopia uses, then each design decision the
mapping raises, with the options and what they trade. [Design.md](Design.md)
records the decisions themselves, and [Using.md](Using.md) shows the API.

## Coroutines in brief

A coroutine is a function that can stop partway through and continue later.
Any function whose body uses `co_await`, `co_yield` or `co_return` is one.

### The pieces

| Piece | What it is |
|---|---|
| Frame | A heap block the compiler allocates for each call. It holds the parameters, the local variables, the promise, and the point where the coroutine stopped. |
| Promise | An object of the type `promise_type`, which the return type names. The compiler calls its member functions at fixed points, and it is where a result is kept. |
| Handle | `std::coroutine_handle<>`, a pointer to a frame. `resume()` continues the coroutine, `destroy()` frees the frame, and `done()` says whether it has finished. |
| Awaiter | The object `co_await` works on. Its three member functions decide whether to stop, what to do on stopping, and what value the `co_await` produces. |
| Return object | What the caller gets back from the call. iopia's is `Task<T>`, which owns the handle. |

### What `co_await` does

The compiler turns `auto value = co_await expression;` into roughly this.

```cpp
auto&& awaiter = expression;            // or expression.operator co_await()
if (!awaiter.await_ready()) {
  // Save the coroutine's state into its frame.
  // Call awaiter.await_suspend(this_coroutine's_handle), and:
  //   returns void          -> stop, and return to whoever resumed us
  //   returns true          -> stop, and return to whoever resumed us
  //   returns false         -> don't stop after all; continue below at once
  //   returns a handle h    -> stop, and resume h in our place
  // <- resume() continues from here
}
auto value = awaiter.await_resume();
```

The awaiter is a temporary, and it lives in the frame from the start of the
`co_await` to the end of it. While the coroutine is stopped, the awaiter stays
at one address. That fact is the bridge to io_uring.

### The promise's fixed points

| The compiler calls | When |
|---|---|
| `get_return_object()` | At the start, to make the value the caller gets back. |
| `initial_suspend()` | Before the body runs. An awaiter that stops makes the coroutine lazy. |
| `return_value(v)` or `return_void()` | At `co_return`. |
| `unhandled_exception()` | When an exception leaves the body. |
| `final_suspend()` | After the body ends. An awaiter that stops keeps the frame alive, so someone can read the result and then `destroy()` it. |

### Who runs what

A coroutine never runs by itself. Some ordinary code calls `resume()`, the
coroutine runs on that caller's stack until it stops, and then `resume()`
returns. Whoever holds a handle decides when the coroutine continues. In
iopia, the event loop holds the handles of stopped coroutines, indirectly,
through `user_data`.

## The mapping

| io_uring | Coroutine | iopia |
|---|---|---|
| Preparing a submission queue entry (SQE) | `await_suspend` | `internal::Awaitable::await_suspend` takes an SQE and prepares it. |
| `user_data` | The awaiter's address | Each awaiter is an `Operation`, and `user_data` points at it. |
| Waiting for completions (`io_uring_enter`) | Code outside any coroutine | `run()` and `Loop::run_once()`. |
| A completion queue entry (CQE) | Resuming the coroutine | The `Operation`'s `complete` stores `res` and calls `resume()`. |
| `res` | `await_resume`'s value | Turned into `std::expected<T, lib::Status>`. |
| The operation's lifetime | The awaiter's lifetime in the frame | Fixed address from `await_suspend` until `await_resume`. |
| Multishot (several completions) | No direct match | Not supported yet. See below. |
| Linked SQEs (`IOSQE_IO_LINK`) | No direct match | Not supported yet. |
| Cancellation (`ASYNC_CANCEL`) | No direct match | Roadmap step 7. |

### One read, step by step

```
task                       loop                          kernel
────                       ────                          ──────
co_await Read{...}
  await_ready -> false
  await_suspend(handle)
    maybe_submission(this) ── SQE, user_data = &awaiter
    io_uring_prep_read
    remember handle
    return true
  (task stops; resume()
   returns to run())
                           run_once
                             submit_and_wait(1) ───────► read runs
                                                         CQE {user_data, res}
                             take_completions ◄────────
                             dispatch(CQE)
                               awaiter.complete(CQE)
                                 result_ = res
  ◄───────────────────────────── handle.resume()
  await_resume -> expected<bytes>
  ... task continues until its next co_await
                           (resume() returns; dispatch continues)
```

The awaiter needs no allocation of its own. The frame already holds it, at a
fixed address, for exactly as long as the kernel holds `user_data`.

## Design decisions

Each section lists the options, what each trades, and what iopia does today.

### 1. Where the awaiter finds its loop

`await_suspend` needs a loop to take an SQE from.

| Option | Trade |
|---|---|
| **Pass it in:** `Read{Depend(loop), ...}` (iopia today) | Explicit and simple, and an awaiter works in any coroutine type. Every call names the loop. |
| **Keep it in the task's promise.** `run()` gives the top task a loop, each child inherits it, and `await_suspend` reads it from the awaiting coroutine's promise. `std::execution::task` does this with its scheduler. | Calls read `co_await Read{file, buffer, 0}`. The awaiter only works inside iopia's `Task`, and the promise grows by a pointer. |
| **A thread-local current loop.** | Shortest calls. Hidden state, and wrong as soon as a task touches two loops. |

### 2. Lazy or eager tasks

| Option | Trade |
|---|---|
| **Lazy** (iopia today): `initial_suspend` stops, and the task starts when awaited or run. | The parent sets its continuation before the child runs, so the child always knows whom to resume. A task can be created and passed around before it starts. |
| **Eager:** the body runs as soon as it's called. | Starts work sooner. A child can finish before its parent awaits it, so the parent and child need a handshake over who resumes whom. A task that is never awaited still runs. |

### 3. How a finished child resumes its parent

When a child task finishes, its parent must continue. The child's
`final_suspend` awaiter decides how.

| Option | Trade |
|---|---|
| **Call `parent.resume()`** from the child's final suspend. | Simple. Each finish adds a stack frame under the one that resumed the child, so a loop over many children grows the stack until it overflows. |
| **Symmetric transfer:** `await_suspend` returns the parent's handle. | The compiler should turn this into a tail call, so the stack stays flat. The standard doesn't require the tail call, and GCC 16 at `-O0` (Bazel's default build) doesn't make it. A test of a million children overflowed the stack there. |
| **Return to the parent's `await_suspend`** (iopia today, for a child that finishes before it stops). The parent starts the child with `resume()`. If the child finishes during that call, its final suspend returns there, and the parent's `await_suspend` returns `false`, so the parent continues without being resumed. | Flat stack on every compiler and optimization level. Needs a flag in the promise to tell the two cases apart. A child that stops on io_uring still uses symmetric transfer when it finishes later. |

### 4. Resuming inline or from a queue

A completion arrives inside `Loop::run_once()`'s dispatch.

| Option | Trade |
|---|---|
| **Resume inline** (iopia today): `complete` calls `resume()` directly. | No extra hop, so the lowest latency. The coroutine runs inside dispatch, until its next `co_await`, before the next completion is handled. |
| **Queue the handle**, and resume queued handles after dispatch. | Every completion in a batch is handled before any coroutine runs, which bounds how long dispatch takes and makes the order fair. Costs a queue and a second pass. |

### 5. How results and errors come back

| Option | Trade |
|---|---|
| **`std::expected<T, lib::Status>` from `await_resume`** (iopia today) | Errors are values and the caller must look at them. A task that just propagates an error writes it out by hand. |
| **Throw from `await_resume`** | Propagation is free in the source. iopia avoids exceptions, and handlers already can't throw. |
| **Return `res` raw** | Cheapest. Every caller repeats the `res < 0` check and the errno mapping. |

Making a `lib::Status` formats a message, so an error costs a string. A
success costs nothing extra.

### 6. When the submission queue is full

`Loop::maybe_submission` already submits what the queue holds and tries once
more. If it still has no SQE, the awaiter must do something.

| Option | Trade |
|---|---|
| **Don't stop, and report `BUSY`** (iopia today): `await_suspend` returns `false`. | Simple, and the caller decides what to do. Every caller needs a plan for `BUSY`. |
| **Stop, and wait for space.** Put the awaiter on a queue the loop drains after each batch. | Callers never see `BUSY`. The loop gains a wait queue, and awaiters gain a second state. |

### 7. Where frames come from

Every task call allocates its frame with `operator new`.

| Option | Trade |
|---|---|
| **The default allocator** (iopia today) | Nothing to write. One allocation and one free per task. |
| **Allocation elision (HALO).** The compiler may put a child's frame inside its parent's when it can prove the child's lifetime is nested. | Free when it happens. Compilers rarely manage it when the task type owns its handle, and nothing guarantees it. |
| **A custom `operator new` in the promise**, taking frames from a per-loop pool. | Fast, and fits one ring per thread. Frame sizes vary, so the pool needs size classes, and a frame must not outlive its pool. |

### 8. Following `std::execution::task`

| Option | Trade |
|---|---|
| **iopia's own `Task<T>`** (iopia today) | Uses only `<coroutine>`, so coroutine users compile no sender code. The two front-ends need a bridge (roadmap step 6). |
| **Model `Task<T>` on `std::execution::task`**, which awaits senders. | One set of operations, as senders, serves both front-ends. Coroutine users compile the sender machinery. |

### 9. Destroying a task that is waiting on io_uring

The kernel holds the address of an awaiter in the task's frame. Destroying the
frame early leaves the kernel pointing at freed memory.

| Option | Trade |
|---|---|
| **Forbid it** (iopia today) | Nothing to write. The rule is the caller's to keep. |
| **Cancel, then wait, in the destructor.** Submit `ASYNC_CANCEL` for the operation and run the loop until its completion arrives. | Always safe. A destructor that runs the loop can call other handlers, at a point the caller didn't expect. |
| **Detach the operation.** Move what the kernel uses into a loop-owned node and let the frame go. | Destruction stays cheap. Every awaiter must be able to hand itself off, and the kernel's buffers must outlive the frame too. |

### 10. Multishot operations

A multishot operation, such as multishot receive or accept, completes many
times, and one `co_await` resumes once.

| Option | Trade |
|---|---|
| **Re-arm a single-shot operation per `co_await`** | Fits today's awaiters. Loses multishot's saving of one submission per completion. |
| **An async stream:** `co_await stream.next()` in a loop. The operation buffers completions that arrive while no one waits. | Keeps multishot's saving. Needs a buffer, and a rule for when it fills. |
| **Handlers, not coroutines** | Already works with `Loop::queue`. Leaves the coroutine style. |

### 11. Exceptions inside tasks

| Option | Trade |
|---|---|
| **Terminate** (iopia today): `unhandled_exception` calls `std::terminate`. | Matches handlers, which can't throw. A bug stops the program at once. |
| **Store the exception** in the promise and rethrow it from the parent's `await_resume`. | Exceptions work across `co_await`. Costs an `std::exception_ptr` per task and a check on every resume. |

## Summary of today's choices

| Question | iopia today |
|---|---|
| Where the awaiter finds its loop | Passed in, `Depend<Loop>` |
| Lazy or eager | Lazy |
| How a child resumes its parent | Returns to the parent's `await_suspend` if it finishes before stopping, symmetric transfer otherwise |
| Inline or queued resumption | Inline, from dispatch |
| Results and errors | `std::expected<T, lib::Status>` |
| A full submission queue | Report `BUSY` without stopping |
| Frames | The default allocator |
| `std::execution::task` | Own `Task<T>`, no sender code |
| Destroying a waiting task | Forbidden |
| Multishot | Not supported yet |
| Exceptions | Terminate |
