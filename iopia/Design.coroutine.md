# io_uring, coroutines and std::execution

This document explains iopia's coroutine front-end for a reader who knows
io_uring better than C++ coroutines. It introduces the three technologies,
lines up their concepts, shows what the compiler does with a coroutine, where
everything lives and for how long, and how each customization point changes
the flow. Then it maps iopia's design onto those concepts and explains its
choices. [Design.md](Design.md) records the decisions, and
[Using.md](Using.md) shows the API.

Contents

- [A. What each one is](#a-what-each-one-is)
- [B. Their core concepts, side by side](#b-their-core-concepts-side-by-side)
- [C. What the compiler turns a coroutine into](#c-what-the-compiler-turns-a-coroutine-into)
- [D. Where each piece lives, and for how long](#d-where-each-piece-lives-and-for-how-long)
- [E. Lifetimes of a parent and child, from ordinary code and back](#e-lifetimes-of-a-parent-and-child-from-ordinary-code-and-back)
- [F. The sequence of a parent and child, from ordinary code and back](#f-the-sequence-of-a-parent-and-child-from-ordinary-code-and-back)
- [G. How each customization point changes the sequence](#g-how-each-customization-point-changes-the-sequence)
- [H. How iopia maps onto these concepts, and the trade-offs](#h-how-iopia-maps-onto-these-concepts-and-the-trade-offs)
- [I. Why iopia chose what it did](#i-why-iopia-chose-what-it-did)

## A. What each one is

**io_uring** is Linux's interface for asynchronous system calls. A program
writes requests into a ring it shares with the kernel, and the kernel writes
results into a second ring. Each request carries a 64-bit `user_data` that
comes back with its result, so the program can tell which request finished.

**C++ coroutines** (C++20) are functions that can stop partway through and
continue later. The compiler turns the function into an object that holds its
state, and code outside the coroutine decides when it continues. C++20
supplies only the machinery. C++23 adds `std::generator`, and C++26 adds
`std::execution::task`.

**`std::execution`** (C++26) is a library model of asynchronous work. A
*sender* describes work, a *receiver* is where its result goes, and
algorithms such as `then` and `when_all` compose senders into bigger ones.

How they relate:

```
                    your code
          ┌───────────────┴────────────────┐
     coroutines                      std::execution
  (write async code               (compose async work
   as straight-line code)          from senders)
          └──── std::execution::task ──────┘
                 (a coroutine that awaits
                  senders and is a sender)
                         │
              something that completes later
                         │
                      io_uring
          (the kernel completes requests, and
           user_data says which one finished)
```

io_uring is the source of asynchronous events. Coroutines and
`std::execution` are two ways to write code that waits on such a source.
Each one turns "do this, then later continue with that" into something a
person can read. `std::execution::task` joins the two.

## B. Their core concepts, side by side

### Each one is a callback, written differently

All three come down to "do some work, and call me back when it finishes".
`co_await x` hands `x` a callback whose body is the rest of the function, and
the coroutine's handle is that callback.

```
callback style                     coroutine style
(a hypothetical async_read)
──────────────                     ───────────────
async_read(fd, buf, [&](int n) {   int n = co_await Read{...};
  ...rest...                       ...rest...
});
```

All four ways below go through the same eight steps.

| Step | Tag | What happens |
|---|---|---|
| Describe the work | `[describe]` | Say which operation, on what, with which buffer |
| Register the callback | `[register]` | Say what runs when it finishes |
| Begin the work | `[begin]` | Hand it to the kernel |
| Call back | `[call back]` | Run what was registered |
| Deliver the result | `[deliver]` | Give the callback the result |
| Cancel | | Stop the work before it finishes |
| Where code runs | | Which thread runs the callback |
| Drive it from ordinary code | `[drive]` | Wait for completions, and dispatch them |

SQE and CQE are the submission and completion queue entries, one per request
and one per result.

### The same read, four ways

Each example reads `size` bytes from `file` at offset 0 into `buffer`, which
is a pointer, or a `std::span` for iopia's `Read`. The tags in the comments
name the steps above, and a table after each example says how it takes each
step.

**io_uring**, through liburing.

```cpp
::io_uring ring;
::io_uring_queue_init(64, &ring, 0);

::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
::io_uring_prep_read(sqe, file, buffer, size, 0);    // [describe]
::io_uring_sqe_set_data64(sqe, 42);                  // [register] a tag
::io_uring_submit(&ring);                            // [begin]

::io_uring_cqe* cqe = nullptr;
::io_uring_wait_cqe(&ring, &cqe);                    // [drive]
if (cqe->user_data == 42) {                          // [call back] by dispatching on the tag
  std::int32_t bytes = cqe->res;                     // [deliver]
}
::io_uring_cqe_seen(&ring, cqe);
```

| Step | io_uring |
|---|---|
| Describe the work | `io_uring_prep_read(sqe, ...)` fills an SQE |
| Register the callback | `io_uring_sqe_set_data64(sqe, 42)` tags the SQE |
| Begin the work | `io_uring_submit` |
| Call back | The program reads a CQE and dispatches on its `user_data` |
| Deliver the result | `cqe->res` |
| Cancel | `IORING_OP_ASYNC_CANCEL` |
| Where code runs | The thread that reaps the CQ |
| Drive it from ordinary code | `io_uring_wait_cqe` |

**Callback style**, through iopia's `Loop::queue`.

```cpp
std::int32_t bytes = 0;
Loop loop{*Ring::make(64)};

bool queued = loop.queue(
    [&](::io_uring_sqe* submission) noexcept {       // [describe]
      ::io_uring_prep_read(submission, file, buffer, size, 0);
    },
    [&](Completion completion) noexcept {             // [register] this lambda
      bytes = completion.result;                      // [call back], [deliver]
    });
auto ran = loop.run();                                // [begin] and [drive]
```

| Step | Callback style (`Loop::queue`) |
|---|---|
| Describe the work | The prepare lambda, which calls `io_uring_prep_read` |
| Register the callback | The handler lambda given to `loop.queue` |
| Begin the work | `loop.run()` submits the SQE |
| Call back | The loop calls the handler |
| Deliver the result | `completion.result` |
| Cancel | Not yet; roadmap step 7 |
| Where code runs | The thread running the loop |
| Drive it from ordinary code | `loop.run()` |

**Coroutine**, through iopia's `Task` and `Read`.

```cpp
auto read_file(Depend<Loop> loop, std::int32_t file,
               std::span<std::byte> buffer)
    -> Task<std::expected<std::uint32_t, lib::Status>> {
  co_return co_await Read{loop, file, buffer, 0};
  //        ^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  //        |        [describe]
  //        await_suspend: [register] the handle, and prepare the SQE
  //        resume(): [call back]; await_resume(): [deliver]
}

Loop loop{*Ring::make(64)};
auto bytes = run(InOut(loop), read_file(Depend(loop), file, buffer));
// [begin] and [drive]: run()'s loop submits the SQE and waits for its CQE
```

| Step | Coroutine (`Task`, `Read`) |
|---|---|
| Describe the work | `Read{loop, file, buffer, 0}` |
| Register the callback | `co_await` calls `await_suspend(handle)`, which keeps the handle and puts `&Read` in `user_data` |
| Begin the work | The loop submits the SQE that `await_suspend` prepared |
| Call back | The loop calls `handle.resume()` |
| Deliver the result | The completion stores `cqe->res` in the awaiter's `result_`. On resuming, `Read::await_resume()` turns it into a `std::expected<std::uint32_t, lib::Status>`, the bytes read becomes the value of the `co_await` expression |
| Cancel | Not yet; roadmap step 7 |
| Where code runs | The thread running the loop |
| Drive it from ordinary code | `run(InOut(loop), task)` |

**`std::execution`**. `read_sender` stands for a sender that reads with
io_uring; iopia's arrives in roadmap step 5.

```cpp
namespace ex = std::execution;

// With algorithms.
auto work = read_sender(scheduler, file, buffer, 0)  // [describe]
          | ex::then([](std::uint32_t bytes) {       // what runs on [call back]
              return bytes;
            });
auto [bytes] = *ex::sync_wait(std::move(work));      // [register], [begin], [drive]

// Spelled out.
struct Receiver {
  using receiver_concept = ex::receiver_t;
  std::uint32_t* bytes = nullptr;

  auto set_value(std::uint32_t count) && noexcept -> void {   // [call back]
    *bytes = count;                                           // [deliver]
  }
  auto set_error(std::error_code) && noexcept -> void {}
  auto set_stopped() && noexcept -> void {}
};

std::uint32_t bytes = 0;
auto sender = read_sender(scheduler, file, buffer, 0);           // [describe]
auto state = ex::connect(std::move(sender), Receiver{&bytes});   // [register]
ex::start(state);                                                // [begin]
// The scheduler's run loop must now [drive] until set_value runs.
```

| Step | `std::execution` |
|---|---|
| Describe the work | A **sender**, `read_sender(...)` |
| Register the callback | `connect(sender, receiver)`, which makes an **operation state** |
| Begin the work | `start(operation_state)` |
| Call back | `set_value`, `set_error` or `set_stopped` on the **receiver** |
| Deliver the result | The arguments to `set_value` or `set_error` |
| Cancel | A **stop token**, and `set_stopped` |
| Where code runs | A **scheduler** |
| Drive it from ordinary code | `sync_wait`, or the scheduler's run loop |

The four differ in who writes the callback. io_uring leaves dispatch on
`user_data` to the program, the callback style takes a lambda, the coroutine
gets its callback from the compiler, and `std::execution` takes a receiver,
which `sync_wait` writes for you.

### Which piece plays which role

| Coroutine piece | Scope | `std::execution` role |
|---|---|---|
| The awaitable expression, such as `Read{...}` | One wait | Sender |
| The awaiter, while the coroutine is stopped | One wait | Operation state |
| The coroutine handle, resumed with the result | One wait | Receiver |
| The promise | The whole call | The receiver's environment (scheduler, stop token, allocator), and, for a task, where it keeps its parent's receiver |

While a coroutine waits, its handle gets called back, so the handle plays the
receiver. The promise holds policy for the whole call, and supplies what
`std::execution` calls the environment.

Seen from outside, a whole task is itself a sender.

| Seen from outside | Coroutine |
|---|---|
| The sender | The task, before it starts |
| The operation state | The coroutine's frame |
| `start` | `resume()` on the lazy task |
| The receiver it completes into | The parent's continuation, kept in the promise |
| `set_value(v)` | `co_return v`, after which `final_suspend` resumes the parent |

### Lazy and eager

A **lazy** coroutine doesn't run when it is called. The call creates its frame
and returns, and the body starts only when something starts it. An **eager**
coroutine runs its body as soon as it is called, until its first stop.

```cpp
Task<int> task = child();   // lazy: nothing in child() has run yet
int value = co_await task;  // now child() starts
```

iopia's `Task`, `std::execution`'s senders and `std::execution::task` are all
lazy.

### Parents and children

When one coroutine `co_await`s another, the awaiting one is the **parent** and
the awaited one is the **child**. The child keeps the parent's handle as its
**continuation**, and uses it to get the parent running again when it
finishes. A child can be a parent too. The top task is started by ordinary
code, such as `run()`, and has no parent.

```cpp
auto child(Depend<Loop> loop) -> Task<std::int32_t> {
  co_await Read{loop, file, buffer, 0};
  co_return 7;
}

auto parent(Depend<Loop> loop) -> Task<std::int32_t> {
  std::int32_t value = co_await child(loop);
  co_return value * 2;
}

auto result = run(InOut(loop), parent(Depend(loop)));
```

### Custody: one owner at a time

At every moment, exactly one party is responsible for a stopped coroutine's
handle, and that party will resume it or destroy it. Every stop hands custody
to someone, such as the kernel through `user_data`, a parent, or a queue.
`std::execution` has the same rule for an operation state: once started, it
stays alive at one address until it calls its receiver exactly once.

| Custody bug | What happens |
|---|---|
| Nobody holds the handle | The coroutine never resumes, and its frame leaks |
| Two parties hold it | It is resumed twice, or after it was destroyed |
| The holder outlives the frame | The kernel completes into freed memory |

### Where coroutines and `std::execution` differ

| | Coroutines | `std::execution` |
|---|---|---|
| Storage | A frame per call, on the heap unless elided | Operation states nest inside each other, sized at compile time, with no allocation |
| Errors | Whatever the promise chooses | Three channels: value, error and stopped |
| Cancellation | Whatever the promise and awaiters choose | Built in, through stop tokens |
| Completing inline | A child that finishes inside its parent's `resume()` can grow the stack | A sender that completes inside `start` can too; schedulers trampoline to bound it |

## C. What the compiler turns a coroutine into

A function becomes a coroutine when its body uses `co_await`, `co_yield` or
`co_return`. The return type names the promise type through
`std::coroutine_traits<Return, Arguments...>::promise_type`, which by default
is `Return::promise_type`.

### C0. The code as written

The rest of this section expands this coroutine, and a caller that uses it.
The comments say which part expands where.

```cpp
// Task<std::int32_t>::promise_type is the promise.
auto child(Depend<Loop> loop, std::int32_t file,      // C1: calling it
           std::span<std::byte> buffer)
    -> Task<std::int32_t> {                            // C2: the body starts here
  auto read = co_await Read{loop, file, buffer, 0};    // C3: co_await
  if (!read) {
    co_return -1;                                      // C5: co_return
  }
  co_return static_cast<std::int32_t>(*read);          // C5
}                                                      // C2: the body ends here
                                                       // C6 splits the body at the co_await

auto parent(Depend<Loop> loop, std::int32_t file) -> Task<std::int32_t> {
  std::array<std::byte, 64> buffer;
  Task<std::int32_t> task = child(loop, file, buffer); // C1
  std::int32_t bytes = co_await std::move(task);       // C3, through Task's operator co_await
  co_return bytes;
}                                                      // task's destructor: C7
```

`Task` has no `co_yield`. A generator shows C4.

```cpp
auto count_to(std::int32_t limit) -> std::generator<std::int32_t> {
  for (std::int32_t n = 1; n <= limit; ++n) {
    co_yield n;                                        // C4: co_yield
  }
}
```

### C1. The call

Calling the coroutine runs roughly this, before any of the body.

```cpp
auto child(Arguments... arguments) -> Task<T> {
  // 1. Allocate the frame. The promise's operator new is used if it has one,
  //    first trying operator new(size, arguments...).
  void* frame = promise_type::operator new(frame_size);
  // If the promise has get_return_object_on_allocation_failure(), the
  // allocation uses nothrow new, and a null frame returns that object instead.

  // 2. Copy the parameters into the frame. By-value parameters are moved or
  //    copied. Reference parameters stay references.
  // 3. Construct the promise in the frame, from the parameter copies if it has
  //    a matching constructor, otherwise by default.
  // 4. Make the object the caller gets back.
  auto return_object = promise.get_return_object();
  // 5. Run the body (C2). The call returns return_object to the caller at the
  //    coroutine's first stop, or when it finishes.
}
```

### C2. The body

```cpp
{
  try {
    co_await promise.initial_suspend();       // lazy coroutines stop here
    /* the function body */
  } catch (...) {
    if (/* initial_suspend's await_resume hasn't run */) throw;
    promise.unhandled_exception();
  }
final_suspend:
  co_await promise.final_suspend();           // must not throw
  // If final_suspend didn't stop, the frame is destroyed here (C7).
}
```

### C3. `co_await expression`

```cpp
// a. The promise may rewrite the expression.
auto&& awaitable = promise.await_transform(expression);  // only if the promise
                                                          // declares await_transform
// b. The awaitable may supply a separate awaiter.
auto&& awaiter = awaitable.operator co_await();  // or a free operator co_await,
                                                 // or the awaitable itself
// c. The three hooks.
if (!awaiter.await_ready()) {
  // Save the state into the frame, and note where to resume.
  auto next = awaiter.await_suspend(this_handle);
  //   void            -> stop, and return to whoever resumed this coroutine
  //   bool true       -> the same
  //   bool false      -> don't stop; continue below at once
  //   handle h        -> stop, and resume h instead (symmetric transfer)
  //   throws          -> don't stop; the exception leaves the co_await
  // <- resume() continues here
}
auto value = awaiter.await_resume();
```

`initial_suspend`, `final_suspend` and `yield_value` results skip step a.

### C4. `co_yield value`

```cpp
co_await promise.yield_value(value);
```

### C5. `co_return`

```cpp
co_return value;   // -> promise.return_value(value); goto final_suspend;
co_return;         // -> promise.return_void();       goto final_suspend;
// Falling off the end -> promise.return_void(), if the promise has it;
//                        otherwise undefined behavior.
```

### C6. The state machine

The compiler splits the body at each stop, and `resume()` jumps to the right
piece.

```cpp
struct child_frame {
  void (*resume)(child_frame*);    // what handle.resume() calls
  void (*destroy)(child_frame*);   // what handle.destroy() calls
  promise_type promise;
  /* parameter copies, locals and temporaries alive across a stop */
  int stopped_at;                  // which co_await it stopped at
};

void child_resume(child_frame* frame) {
  switch (frame->stopped_at) {
    case 0: /* after initial_suspend */ ...
    case 1: /* after the first co_await */ ...
  }
}
```

### C7. Destroying a frame

```cpp
handle.destroy();
// 1. Destroy the locals and temporaries alive at the point where it stopped.
// 2. Destroy the promise.
// 3. Destroy the parameter copies.
// 4. Free the frame with the promise's operator delete, if it has one.
```

## D. Where each piece lives, and for how long

| Piece | Lives in | From | Until |
|---|---|---|---|
| Frame | The heap, unless elided into the caller's frame (HALO) | The call | `handle.destroy()`, or the end of `final_suspend` if it doesn't stop |
| Promise | The coroutine's own frame | The call | The frame's destruction |
| Parameter copies | The coroutine's own frame | The call | The frame's destruction |
| Reference parameters | Wherever the caller's object is | The caller decides | The caller's object's lifetime, which can end before the coroutine does |
| Locals | The frame, if alive across a stop; otherwise the stack | Their declaration | Their scope's end |
| An awaiter for `co_await Read{...}` | The awaiting coroutine's frame | The `co_await` | The end of the full-expression, usually the statement |
| An awaiter that is a named variable | The awaiting coroutine's frame | Its declaration | Its scope's end |
| The return object (`Task`) | Wherever the caller puts it | The call | The caller's choice; iopia's `Task` destroys the frame with it |
| The handle | Anywhere; it is one pointer | `from_promise`, or the frame's creation | Valid until the frame is destroyed |
| The result | The promise | `return_value` | Read by the parent's `await_resume`, then destroyed with the frame |
| io_uring's `user_data` | The kernel | `await_suspend` prepares the SQE | The CQE |

A frame laid out by a typical compiler:

```
child's frame
┌───────────────────────────────────────┐
│ resume function pointer               │
│ destroy function pointer              │
│ promise                               │  continuation_, value_, starting_
│ parameter copies                      │
│ locals and temporaries alive at stops │  including an awaiter such as Read
│ which point it stopped at             │
└───────────────────────────────────────┘
```

The standard lets a compiler put a child's frame inside its parent's when it
can prove the child never outlives the parent (heap allocation elision,
HALO). It permits this and never requires it. A parent that awaits ten
children, with iopia's `Task`:

| Build | Allocations |
|---|---|
| GCC or Clang, at `-O0` or `-O2` | 11, one per frame |
| Clang at `-O0`, with `[[clang::coro_await_elidable]]` on `Task` | 11 |
| Clang at `-O2`, with `[[clang::coro_await_elidable]]` on `Task` | 1, every child inside the parent |

## E. Lifetimes of a parent and child, from ordinary code and back

The example from B, run from `main`.

```cpp
int main() {
  Loop loop{*Ring::make(64)};
  auto result = run(InOut(loop), parent(Depend(loop)));
}
```

| Time | Event |
|---|---|
| t0 | `main` calls `parent(...)`: the parent's frame and promise are created, and the lazy parent stops at `initial_suspend` |
| t1 | `run` resumes the parent; the parent calls `child(...)`: the child's frame is created, and it stops at `initial_suspend` |
| t2 | The parent awaits the child; the child starts and awaits `Read`, which takes an SQE and stops |
| t3 | The kernel finishes the read; its CQE resumes the child |
| t4 | The child returns 7 and reaches `final_suspend`, which resumes the parent |
| t5 | The parent's statement ends: the child's `Task` is destroyed, and with it the child's frame |
| t6 | The parent returns 14 and reaches `final_suspend`; `run` reads the value |
| t7 | `run` returns, destroying the parent's `Task` and frame |

```
              t0    t1    t2    t3    t4    t5    t6    t7
main          ██████████████████████████████████████████████
run()               ████████████████████████████████████████
parent frame  ██████████████████████████████████████████
child frame         ████████████████████████
Read awaiter              ████████████
SQE in kernel             ██████
```

## F. The sequence of a parent and child, from ordinary code and back

```
main      run()       parent          child           loop/kernel
 │ parent()           │               │                │
 │──────────────────► create frame, initial_suspend: stop
 │◄───────── Task ────│               │                │
 │ run(loop, Task)    │               │                │
 │─────────►│ resume()│               │                │
 │          │────────►│ child()       │                │
 │          │         │──────────────►create frame, stop
 │          │         │◄──── Task ────│                │
 │          │         │ co_await Task: await_suspend   │
 │          │         │ continuation = parent          │
 │          │         │ resume() ────►│                │
 │          │         │               │ co_await Read: │
 │          │         │               │ take SQE ─────►│ user_data = &Read
 │          │         │               │ stop           │
 │          │         │◄──────────────│ (resume returns)
 │          │         │ child not done: stop           │
 │          │◄────────│ (resume returns)               │
 │          │ run_once ──────────────────────────────►│ submit and wait
 │          │         │               │                │ read finishes
 │          │         │               │◄── CQE: resume │
 │          │         │               │ co_return 7    │
 │          │         │               │ final_suspend: │
 │          │         │◄─ symmetric transfer ─│        │
 │          │         │ value = 7     │                │
 │          │         │ destroy child frame            │
 │          │         │ co_return 14  │                │
 │          │         │ final_suspend: continuation is noop
 │          │◄──────────────── (dispatch, run_once return)
 │          │ done: read 14           │                │
 │◄─── 14 ──│ destroy parent frame    │                │
```

## G. How each customization point changes the sequence

Most to least commonly customized.

### G1. `await_suspend`'s return value

```
returns true or void           returns false               returns handle h
────────────────────           ─────────────               ────────────────
coroutine stops                coroutine never stops       coroutine stops
resume() returns to            continues at once with      h.resume() runs in its
whoever resumed it             await_resume()              place, as a tail call
                                                           when the compiler makes one
used: started async work       used: work finished         used: hand over to another
(iopia's Read)                 synchronously, or no SQE    coroutine (a parent)
```

### G2. `initial_suspend`: lazy or eager

```
lazy (suspend_always)                   eager (suspend_never)
caller ── child() ──► frame, stop       caller ── child() ──► frame, body runs
caller ◄── Task ─────                   ...until the first stop
caller ── co_await ──► body runs        caller ◄── Task ─────
                                        caller ── co_await ──► finished already?
                                        needs a handshake over who resumes whom
```

### G3. `final_suspend`

```
returns the continuation         suspend_always                suspend_never
(symmetric transfer)             (and the owner destroys it)   (self-destroys)
child ──► parent resumes         child stops; resume()         child's frame is freed;
                                 returns to its resumer        resume() returns
used: tasks                      used: generators, run()       used: fire-and-forget
                                 reading the result            coroutines, no result
```

### G4. `await_ready`

```
returns true: skip the stop entirely, go straight to await_resume.
used: the result is already available, so no frame save and no custody handoff.
```

### G5. `return_value` and `unhandled_exception`

```
co_return v ──► promise.return_value(v) stores v ──► final_suspend
throw       ──► promise.unhandled_exception()
                  stores it, to rethrow in the parent's await_resume
                  or calls std::terminate (iopia)
```

### G6. `await_transform`

```
co_await x ──► promise.await_transform(x) ──► the awaiter actually used
used: std::execution::task turns every sender into an awaiter, then resumes
      on its own scheduler (an extra hop when the completion ran elsewhere):

task ── co_await sender ──► sender completes on another thread
                             ──► schedule back onto the task's scheduler ──► task resumes
```

### G7. `yield_value`

```
consumer                       generator
 │ ++iterator ─ resume() ─────► runs to co_yield v
 │                              yield_value(v): store v, stop
 │◄─────────────────────────── resume returns
 │ reads v                      (repeats until co_return)
```

### G8. `operator co_await`

Changes which object is the awaiter, and leaves the sequence as it is.
iopia's `Task` uses it, so `co_await task` gets a dedicated awaiter that
starts the child.

### G9. Allocation: `operator new`, `get_return_object_on_allocation_failure`

Changes where the frame comes from, and leaves the sequence as it is.

## H. How iopia maps onto these concepts, and the trade-offs

| io_uring | Coroutine | `std::execution` | iopia |
|---|---|---|---|
| Prepare an SQE | `await_suspend` | `start` | `internal::Awaitable::await_suspend` takes an SQE and prepares it |
| `user_data` | The awaiter's address | The operation state's address | Each awaiter is an `Operation`, and `user_data` points at it |
| `io_uring_enter` | Code outside any coroutine | `sync_wait` | `run()` and `Loop::run_once()` |
| A CQE | `resume()` | Calling the receiver | `Operation::complete` stores `res` and calls `resume()` |
| `res` | `await_resume`'s value | `set_value` or `set_error` | `std::expected<T, lib::Status>` |
| The operation's lifetime | The awaiter's lifetime | The operation state's lifetime | From `await_suspend` to `await_resume` |
| Multishot | No direct match | No direct match | Not supported yet |
| Linked SQEs | No direct match | Sequencing, such as `let_value` | Not supported yet |
| Cancellation | No direct match | Stop tokens | Roadmap step 7 |

iopia's `Read` is the awaitable, its own awaiter and the `Operation` at once.
A sender front end splits it: a copyable sender, and a fixed-address
operation state that `connect` builds.

Each decision below lists its options and what they trade, with iopia's
choice in bold. Section I gives the reasons.

| # | Question | Options and trades |
|---|---|---|
| 1 | Where the awaiter finds its loop | **Passed in**: explicit; works in any coroutine type; every call names the loop. *In the promise*: shorter calls; the awaiter works only in iopia's `Task`. *Thread-local*: shortest; hidden, and wrong with two loops. |
| 2 | Lazy or eager | **Lazy**: the parent sets its continuation before the child runs. *Eager*: starts sooner; needs a handshake, and runs even if never awaited. |
| 3 | How a finished child resumes its parent | *`resume()`*: grows the stack. *Symmetric transfer*: flat if the compiler makes a tail call. **Return to the parent's `await_suspend` when finished before stopping, symmetric transfer otherwise**: flat everywhere; a flag in the promise. |
| 4 | Resume inline or queued | **Inline**: lowest latency; the coroutine runs inside dispatch. *Queued*: bounded, fair dispatch; a queue and a second pass. |
| 5 | Results and errors | **`std::expected<T, lib::Status>`**: explicit; propagation by hand. *Throw*: free propagation; iopia avoids exceptions. *Raw `res`*: cheapest; every caller maps errno. |
| 6 | A full submission queue | **Don't stop, report `BUSY`**: simple; callers need a plan. *Wait for space*: callers never see it; a wait queue in the loop. |
| 7 | Where frames come from | **Default `operator new`**: nothing to write. *HALO*: free when it happens, rare without help. *A pool in the promise*: fast; size classes, and frames must not outlive it. |
| 8 | Follow `std::execution::task` | **Own `Task`**: no sender code for coroutine users; needs a bridge. *Model it*: one set of operations; coroutine users compile senders. |
| 9 | Destroying a task that waits on io_uring | **Forbid it**: nothing to write. *Cancel and wait in the destructor*: safe; the destructor runs the loop. *Detach the operation*: cheap; every awaiter must hand itself off. |
| 10 | Multishot | *Re-arm per `co_await`*: fits today's awaiters. *An async stream*: keeps multishot's savings; needs a buffer. *Handlers*: already works with `Loop::queue`. |
| 11 | Exceptions in tasks | **Terminate**: matches handlers. *Store and rethrow*: exceptions across `co_await`; an `exception_ptr` per task. |

## I. Why iopia chose what it did

### 1. The loop is passed in

```cpp
// Passed in (iopia): works in any coroutine type.
co_await Read{loop, file, buffer, 0};

// In the promise: await_suspend must name iopia's promise to find the loop.
template <typename ValueType>
auto await_suspend(std::coroutine_handle<internal::Promise<ValueType>> awaiting)
    -> bool { Loop& loop = awaiting.promise().loop(); /* ... */ }
```

The awaiter takes a plain `std::coroutine_handle<>`, so it never depends on
iopia's `Task`. Moving the loop into the promise stays open as a later
convenience.

### 2. Tasks are lazy

```
lazy:   parent sets continuation ──► then starts child ──► child can always resume it
eager:  child may finish ──► before parent has a continuation to give ──► handshake
```

### 3. A child that finishes early returns to its parent's `await_suspend`

```
a loop over children that finish without stopping, with plain resume():
  parent ─ resume ─► child1 ─ resume ─► parent ─ resume ─► child2 ─ ...   stack grows
  1,000,000 children overflowed GCC 16 at -O0, where symmetric transfer isn't a tail call

iopia:
  parent.await_suspend ─ resume ─► child1 finishes ─► returns false ─► parent continues
  stack stays flat on every compiler and optimization level
```

```cpp
auto await_suspend(std::coroutine_handle<> awaiting) noexcept -> bool {
  promise.continuation_ = awaiting;
  promise.starting_ = true;
  coroutine.resume();                 // the child runs until it stops or finishes
  promise.starting_ = false;
  return !coroutine.done();           // finished: don't stop the parent
}
```

### 4. Completions resume inline

```
CQE ──► dispatch ──► Operation::complete ──► handle.resume() ──► coroutine runs
no queue, no second pass: the coroutine continues in the same call as the completion
```

### 5. Results are `std::expected`

```cpp
auto read = co_await Read{loop, file, buffer, 0};
if (!read) {
  co_return Failure{std::move(read).error()};   // propagation is visible
}
```

iopia doesn't use exceptions, and handlers already can't throw.

### 6. A full queue reports `BUSY`

```
await_suspend ──► try_take_submission ──► null ──► return false ──► await_resume: BUSY
the caller decides: retry, back off, or fail
```

### 7. Frames use the default allocator

No allocator to write yet. Clang's `[[clang::coro_await_elidable]]` cut ten
child frames to none at `-O2` (D), which makes it the cheapest next step.

### 8. iopia's own `Task`

```
program using only coroutines:  task.hpp + awaitable.hpp   no sender code compiled
program using only senders:     (step 5) sender headers     no coroutine task compiled
both:                           (step 6) a bridge
```

### 9. Destroying a waiting task is forbidden

```
destroy frame ──► Read awaiter freed ──► kernel still has user_data = &Read ──► CQE ──► use after free
```

Cancellation (roadmap step 7) is the tool any safe alternative needs, so the
rule waits for it.

### 10. Multishot waits

Each option needs either new awaiters or a buffer policy. Handlers already
cover multishot through `Loop::queue`.

### 11. Exceptions terminate

```cpp
[[noreturn]] auto unhandled_exception() const noexcept -> void {
  std::terminate();   // the same rule as a handler's escaping exception
}
```
