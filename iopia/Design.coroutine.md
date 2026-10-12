# io_uring and C++ coroutines

This document explains how io_uring's concepts map onto C++ coroutines, for a
reader who knows io_uring better than coroutines. It covers the coroutine
machinery first, then a small model for reasoning about it, with C++26's
`std::execution` terms beside the coroutine ones, then the mapping iopia
uses, then each design decision the mapping raises, with the options and what
they trade. [Design.md](Design.md)
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
| Return object | What the caller gets back from the call. Its type, the coroutine's return type, names the promise type, so it decides how the coroutine behaves. iopia's is `Task<T>`, which owns the handle. |

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

### Parents and children

When one task `co_await`s another task, the awaiting task is the **parent**
and the awaited task is the **child**.

```cpp
auto child(Depend<Loop> loop) -> Task<std::int32_t> {
  co_await Read{loop, file, buffer, 0};        // may stop on io_uring
  co_return 7;
}

auto parent(Depend<Loop> loop) -> Task<std::int32_t> {
  std::int32_t value = co_await child(loop);   // the parent awaits the child
  co_return value * 2;
}

run(InOut(loop), parent(Depend(loop)));        // run() starts the top task
```

```
run()            drives the top task, which has no parent
 └─ parent       co_await child(loop)
     └─ child    co_await Read{...}, which stops on io_uring
```

Tasks are lazy, so the parent starts the child. The child's promise keeps the
parent's handle as its **continuation**, and when the child finishes it uses
that handle to get the parent running again. A child can be a parent too, so
the chain can be any depth. The top task's continuation is
`std::noop_coroutine()`, a handle whose `resume()` does nothing, so when the
top task finishes, control returns to `run()`.

## How to think about the customization points

The standard specifies the coroutine machinery hook by hook, in the order the
compiler calls them, and leaves the model to the reader. Four ideas organize
the hooks. Each idea also has a name in C++26's `std::execution`, which solves
the same problem with senders and receivers, so this section gives both. Most
of the model comes from Lewis Baker's "Asymmetric Transfer" articles and the
work that led to `std::execution`.

### A coroutine is a callback the compiler writes

`co_await x` means "give `x` a callback, and the callback is the rest of this
function". The coroutine's handle is that callback, and `resume()` calls it.

```
callback style                    coroutine style
──────────────                    ───────────────
read(fd, buf, [&](int n) {        int n = co_await Read{...};
  ...rest...                      ...rest...
});
```

`std::execution` names the callback a **receiver**. Its three member
functions, `set_value`, `set_error` and `set_stopped`, are the three ways an
operation can call back. io_uring has the same shape: an SQE with `user_data`
registers a callback, and a CQE calls it.

| Idea | io_uring | Callback style | Coroutine | `std::execution` |
|---|---|---|---|---|
| Describe the work | Prepare an SQE | The arguments to `read(...)` | The awaitable, such as `Read{...}` | A **sender** |
| Register the callback | `user_data` | Pass the lambda | `await_suspend(handle)` | **`connect(sender, receiver)`**, which makes an **operation state** |
| Begin the work | Submit | The call itself | Inside `await_suspend` | **`start(operation_state)`** |
| Call back | A CQE | Call the lambda | `handle.resume()` | `set_value`, `set_error` or `set_stopped` on the receiver |
| Deliver the result | `res` | The lambda's argument | `await_resume()` | The arguments to `set_value` or `set_error` |

So designing coroutine support mostly means designing a callback API. Who
calls back, on which thread, inline or deferred, how many times, and how
errors arrive. io_uring already answers those for its operations, which is
why the mapping below is so direct.

### Two scopes: the whole call, and one wait

Each customization point belongs to one of two objects, and the object tells
you its scope.

| Object | Scope | It decides | `std::execution` |
|---|---|---|---|
| **Promise**, chosen by the return type | The whole call, from start to finish | How the coroutine starts and ends, where its value and errors go, where its frame lives, and what it allows inside it | The **environment** a receiver carries (`get_env`: the scheduler, the stop token, the allocator), and `std::execution::task`'s promise |
| **Awaiter**, one per `co_await` | One wait | Whether to stop, whom to hand the callback to, and what value comes back | The **operation state** that `connect` makes, one per operation |

Lifecycle policy goes in the promise, and wait policy goes in the awaiter.
"Resume on the loop that started me" is lifecycle policy. "Submit an SQE" is
wait policy.

### The lifecycle, one hook per transition

The promise's hooks sit on a five-state lifecycle.

```
 create ──► start ──► (stop ⇄ resume)* ──► finish ──► destroy
   │          │              │                │           │
get_return_ initial_    each co_await    return_value,  handle
object,     suspend     (the awaiter's   unhandled_     .destroy()
operator new            three hooks)     exception,
                                         final_suspend
```

| Transition | Hook | The question it answers | `std::execution` |
|---|---|---|---|
| create | `operator new`, `get_return_object` | Where does the frame live, and what does the caller hold? | `connect` builds the operation state, inside the caller's own operation state, with no allocation |
| start | `initial_suspend` | Does the body run now, or wait to be started? | Senders are always lazy: nothing runs until `start` |
| each wait | `await_transform` (optional) | Does the promise rewrite every `co_await` in its body? | `std::execution::task` uses it to await senders and to come back on its scheduler |
| finish | `return_value`, `return_void`, `unhandled_exception` | Where do the result and errors go? | `set_value` and `set_error` on the receiver |
| finish | `final_suspend` | Who runs next, and who frees the frame? | The receiver's completion runs next; the operation state is freed by whoever owns it |

### Custody: one owner at a time

At every moment, exactly one party is responsible for a stopped coroutine's
handle, and that party will resume it or destroy it. Every hook that stops the
coroutine transfers that custody, and `await_suspend` is where the transfer
happens.

| `await_suspend` returns | Custody goes to | What runs next on this thread |
|---|---|---|
| `void` or `true` | Whoever `await_suspend` gave the handle to: the kernel through `user_data`, a queue, another task | Whoever resumed the coroutine gets control back |
| `false` | Nobody, because the coroutine never really stopped | The coroutine, at once |
| a handle `h` | Whoever `await_suspend` gave the handle to | `h` (symmetric transfer) |

`std::execution` states the same rule for an operation state. Once `start`
is called, the operation state must stay alive, at a fixed address, until it
calls one of its receiver's three functions, exactly once. iopia's
`Operation` follows the same rule, which is why an awaiter and a sender's
operation state can both be one.

Most coroutine bugs are custody bugs.

- **Nobody holds the handle.** The coroutine never resumes, and its frame
  leaks.
- **Two parties hold it.** It is resumed twice, or resumed after being
  destroyed.
- **The holder outlives the frame.** The kernel still holds `user_data` when
  a waiting task is destroyed (decision 9).

### A procedure for a new design

1. **Write down the primitive's callback contract.** Who calls back, on which
   thread, inline or deferred, and how many times. For io_uring, the loop
   calls back once per CQE (more for multishot), inline, on the ring's thread.
2. **Write the awaiter from that contract.** `await_suspend` gives the handle
   to whatever calls back, and its return type comes from the custody table.
   In `std::execution` terms, this is writing the sender and its operation
   state.
3. **Answer the lifecycle table in the promise.** Choose lazy or eager first,
   since it decides whether a parent can give the child its continuation
   before the child runs. Then decide who frees the frame, then where results
   and errors go.
4. **Check custody at every arrow** of the lifecycle. An arrow that leaves no
   owner, or two, is a bug.

The remaining hooks (`yield_value`, `operator co_await`,
`get_return_object_on_allocation_failure`) are extensions to reach for when a
goal calls for them.

### Where coroutines and `std::execution` differ

| | Coroutines | `std::execution` |
|---|---|---|
| Storage | A heap frame per call, unless the compiler elides it | Operation states nest inside each other, sized at compile time, with no allocation |
| Errors | Whatever the promise chooses; iopia uses `std::expected` | Three channels: value, error and stopped |
| Cancellation | Whatever the promise and awaiters choose | Built in: a stop token in the environment, and `set_stopped` |
| Inline completion | A child that finishes inside its parent's `resume()` can grow the stack (decision 3) | A sender that completes inside `start` can grow the stack in the same way, and schedulers trampoline to bound it |
| Running from ordinary code | `run(InOut(loop), task)` | `sync_wait(sender)` |

## The mapping

| io_uring | Coroutine | `std::execution` | iopia |
|---|---|---|---|
| Preparing a submission queue entry (SQE) | `await_suspend` | `start` on the operation state | `internal::Awaitable::await_suspend` takes an SQE and prepares it. |
| `user_data` | The awaiter's address | The operation state's address | Each awaiter is an `Operation`, and `user_data` points at it. |
| Waiting for completions (`io_uring_enter`) | Code outside any coroutine | A run loop, or `sync_wait` | `run()` and `Loop::run_once()`. |
| A completion queue entry (CQE) | Resuming the coroutine | Calling the receiver | The `Operation`'s `complete` stores `res` and calls `resume()`. |
| `res` | `await_resume`'s value | `set_value` or `set_error` | Turned into `std::expected<T, lib::Status>`. |
| The operation's lifetime | The awaiter's lifetime in the frame | The operation state's lifetime | Fixed address from `await_suspend` until `await_resume`. |
| Multishot (several completions) | No direct match | No direct match; a sender completes once | Not supported yet. See below. |
| Linked SQEs (`IOSQE_IO_LINK`) | No direct match | Sequencing, such as `let_value` | Not supported yet. |
| Cancellation (`ASYNC_CANCEL`) | No direct match | A stop token, and `set_stopped` | Roadmap step 7. |

### One read, step by step

```
task                       loop                          kernel
────                       ────                          ──────
co_await Read{...}
  await_ready -> false
  await_suspend(handle)
    try_take_submission(this) ── SQE, user_data = &awaiter
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

*Scope: is finding the loop wait policy (the awaiter) or lifecycle policy (the promise)?*

`await_suspend` needs a loop to take an SQE from.

| Option | Trade |
|---|---|
| **Pass it in:** `Read{Depend(loop), ...}` (iopia today) | Explicit and simple. `await_suspend` takes a `std::coroutine_handle<>` and never looks at the promise, so an awaiter works in a coroutine of any return type, such as iopia's `Task`, `std::generator` or another library's task. Every call names the loop. |
| **Keep it in the task's promise.** `run()` gives the top task a loop, each child inherits it, and `await_suspend` reads it from the awaiting coroutine's promise. `std::execution::task` does this with its scheduler. | Calls read `co_await Read{file, buffer, 0}`. `await_suspend` must name iopia's promise type to reach the loop, so the awaiter only works inside iopia's `Task`, and the promise grows by a pointer. |
| **A thread-local current loop.** | Shortest calls. Hidden state, and wrong as soon as a task touches two loops. |

### 2. Lazy or eager tasks

*Lifecycle: the start transition, `initial_suspend`.*

| Option | Trade |
|---|---|
| **Lazy** (iopia today): `initial_suspend` stops, and the task starts when awaited or run. | The parent sets its continuation before the child runs, so the child always knows whom to resume. A task can be created and passed around before it starts. |
| **Eager:** the body runs as soon as it's called. | Starts work sooner. A child can finish before its parent awaits it, so the parent and child need a handshake over who resumes whom. A task that is never awaited still runs. |

### 3. How a finished child resumes its parent

*Custody: at the finish transition, who gets the parent's handle, and on which stack.*

When a child task finishes, its parent must continue. The child's
`final_suspend` awaiter decides how.

| Option | Trade |
|---|---|
| **Call `parent.resume()`** from the child's final suspend. | Simple. Each finish adds a stack frame under the one that resumed the child, so a loop over many children grows the stack until it overflows. |
| **Symmetric transfer:** `await_suspend` returns the parent's handle. | The compiler should turn this into a tail call, so the stack stays flat. The standard doesn't require the tail call, and GCC 16 at `-O0` (Bazel's default build) doesn't make it. A test of a million children overflowed the stack there. |
| **Return to the parent's `await_suspend`** (iopia today, for a child that finishes before it stops). The parent starts the child with `resume()`. If the child finishes during that call, its final suspend returns there, and the parent's `await_suspend` returns `false`, so the parent continues without being resumed. | Flat stack on every compiler and optimization level. Needs a flag in the promise to tell the two cases apart. A child that stops on io_uring still uses symmetric transfer when it finishes later. |

### 4. Resuming inline or from a queue

*Callback contract: does the callback run inline or deferred?*

A completion arrives inside `Loop::run_once()`'s dispatch.

| Option | Trade |
|---|---|
| **Resume inline** (iopia today): `complete` calls `resume()` directly. | No extra hop, so the lowest latency. The coroutine runs inside dispatch, until its next `co_await`, before the next completion is handled. |
| **Queue the handle**, and resume queued handles after dispatch. | Every completion in a batch is handled before any coroutine runs, which bounds how long dispatch takes and makes the order fair. Costs a queue and a second pass. |

### 5. How results and errors come back

*Lifecycle: the finish transition, and `await_resume`'s value.*

| Option | Trade |
|---|---|
| **`std::expected<T, lib::Status>` from `await_resume`** (iopia today) | Errors are values and the caller must look at them. A task that just propagates an error writes it out by hand. |
| **Throw from `await_resume`** | Propagation is free in the source. iopia avoids exceptions, and handlers already can't throw. |
| **Return `res` raw** | Cheapest. Every caller repeats the `res < 0` check and the errno mapping. |

Making a `lib::Status` formats a message, so an error costs a string. A
success costs nothing extra.

### 6. When the submission queue is full

*Custody: `await_suspend` returning `false` keeps the handle.*

`Loop::try_take_submission` already submits what the queue holds and tries once
more. If it still has no SQE, the awaiter must do something.

| Option | Trade |
|---|---|
| **Don't stop, and report `BUSY`** (iopia today): `await_suspend` returns `false`. | Simple, and the caller decides what to do. Every caller needs a plan for `BUSY`. |
| **Stop, and wait for space.** Put the awaiter on a queue the loop drains after each batch. | Callers never see `BUSY`. The loop gains a wait queue, and awaiters gain a second state. |

### 7. Where frames come from

*Lifecycle: the create transition, `operator new`.*

Every task call allocates its frame with `operator new`.

| Option | Trade |
|---|---|
| **The default allocator** (iopia today) | Nothing to write. One allocation and one free per task. |
| **Allocation elision (HALO).** The compiler may put a child's frame inside its parent's when it can prove the child's lifetime is nested. | Free when it happens. Compilers rarely manage it when the task type owns its handle, and nothing guarantees it. |
| **A custom `operator new` in the promise**, taking frames from a per-loop pool. | Fast, and fits one ring per thread. Frame sizes vary, so the pool needs size classes, and a frame must not outlive its pool. |

### 8. Following `std::execution::task`

*Scope: what the promise allows inside it, through `await_transform`.*

| Option | Trade |
|---|---|
| **iopia's own `Task<T>`** (iopia today) | Uses only `<coroutine>`, so coroutine users compile no sender code. The two front-ends need a bridge (roadmap step 6). |
| **Model `Task<T>` on `std::execution::task`**, which awaits senders. | One set of operations, as senders, serves both front-ends. Coroutine users compile the sender machinery. |

### 9. Destroying a task that is waiting on io_uring

*Custody: the kernel holds the handle when the frame goes away.*

The kernel holds the address of an awaiter in the task's frame. Destroying the
frame early leaves the kernel pointing at freed memory.

| Option | Trade |
|---|---|
| **Forbid it** (iopia today) | Nothing to write. The rule is the caller's to keep. |
| **Cancel, then wait, in the destructor.** Submit `ASYNC_CANCEL` for the operation and run the loop until its completion arrives. | Always safe. A destructor that runs the loop can call other handlers, at a point the caller didn't expect. |
| **Detach the operation.** Move what the kernel uses into a loop-owned node and let the frame go. | Destruction stays cheap. Every awaiter must be able to hand itself off, and the kernel's buffers must outlive the frame too. |

### 10. Multishot operations

*Callback contract: how many times the callback runs.*

A multishot operation, such as multishot receive or accept, completes many
times, and one `co_await` resumes once.

| Option | Trade |
|---|---|
| **Re-arm a single-shot operation per `co_await`** | Fits today's awaiters. Loses multishot's saving of one submission per completion. |
| **An async stream:** `co_await stream.next()` in a loop. The operation buffers completions that arrive while no one waits. | Keeps multishot's saving. Needs a buffer, and a rule for when it fills. |
| **Handlers, not coroutines** | Already works with `Loop::queue`. Leaves the coroutine style. |

### 11. Exceptions inside tasks

*Lifecycle: the finish transition, `unhandled_exception`.*

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
