# frt - Flössie's ready (FreeRTOS) threading

frt is an object-oriented wrapper around FreeRTOS tasks, queues, mutexes, and semaphores. It provides the basic tools for a clean multithreading approach based on the [Arduino_FreeRTOS_Library](https://github.com/feilipu/Arduino_FreeRTOS_Library) with focus on static allocation, so you know your SRAM demands at compile time.

This will compile just fine with the stock [Arduino_FreeRTOS_Library](https://github.com/feilipu/Arduino_FreeRTOS_Library), but if you want the advantages of static allocation you are welcome to try my [`minimal-static`](https://github.com/Floessie/Arduino_FreeRTOS_Library/tree/minimal-static) branch with frt.

**This is work in progress, so take it with a grain of salt...**

## Examples

There's a single example called [`Blink_AnalogRead.ino`](https://github.com/Floessie/frt/blob/master/examples/Blink_AnalogRead/Blink_AnalogRead.ino) for now. [`frt.h`](https://github.com/Floessie/frt/blob/master/src/frt.h) itself isn't that big and magical as well, so take it as a reference until I've spent some more time on documentation.

## API

The whole API resides in the `frt` namespace.

### Task

A task (or thread in other OSes) is the thing that does the work. It's like your common `loop()` workhorse, but with a real OS you can have many of those `loop()` functions running concurrently. A single task is basically implemented like so:

```c++
class MyFirstTask :
    public frt::Task<MyFirstTask>
{
public:
    bool run()
    {
        // Do something meaningful here...

        return true;
    }
};
```

The repetition of `MyFirstTask` in `frt::Task<MyFirstTask>` is due to [static dispatch per CRTP](https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern#Static_polymorphism), a commonly used pattern that saves code and precious RAM. Think of `run()` as your previously used `loop()` function with an additional return value, with which you can signal if you want to be called again or not. If you would return `false` in the example above, `run()` would only be called once.

Things you can only do inside your task class:
* `yield()`: If there's another task with the same or higher priority, switch over to it voluntarily.
* `nap()`: Sleep the shortest possible time.
* `msleep(milliseconds)`: Sleep for some milliseconds. This suspends the task so that others can run.
  - Be aware that there's a [timer granularity](https://github.com/feilipu/Arduino_FreeRTOS_Library#general) of usually 15 milliseconds. So giving for example `14` here won't sleep but simply yield, and giving `20` will only sleep for 15 milliseconds. If you want to definitely sleep for a short time use `nap()`, otherwise see the next function to work-around this.
* `msleep(milliseconds, remainder)`: Sleep for some milliseconds and store the remaining milliseconds in `remainder`. This will result in some jitter but won't loose milliseconds to the timer granularity.
* `wait()`: Wait for a [*direct to task notification*](https://www.freertos.org/RTOS_Task_Notification_As_Binary_Semaphore.html). Some other task must call `post()` to wake you up again.
* `wait(milliseconds)`: Same as above, but with a timeout. Returns `true` if someone `post()`ed you, or `false` on timeout.
* `wait(milliseconds, remainder)`: Same as above but with the `remainder` mechanism on timeout.

Functions that can be called from outside:
* `start(priority)`: Start the task with a certain priority (higher number = higher priority).
  - Note that there's a limited number of available priorities. Stock Arduino_FreeRTOS_Library supports 0-3, my [`minimal-static`](https://github.com/Floessie/Arduino_FreeRTOS_Library/tree/minimal-static) branch 0-7.
  - The idle task that executes `loop()` has priority 0.
* `stop()`: Stops the task.
  - This blocks until the task left its `run()` function, so if you're blocking there indefinitely, `stop()` will never return.
  - If you want to stop from within your task, return `false` from `run()`. Don't call `stop()`!
  - Stopping a task is harder than you might think. Be sure to block with timeouts (on `wait()`, semaphores, and queues) in `run()`.
* `isRunning()`: Returns true if the task is started.
* `getUsedStackSize()`: Each task has a buffer that is used for storing function local variables and return addresses. This function lets you determine the maximum number of bytes used (so far).
  - Only valid while the task is running.
  - Interrupts are also served in a task's context, so the result may vary. Don't be too conservative.
* `post()`: Wake task via *direct to task notification*.
* `preparePostFromInterrupt()`: When posting from an interrupt, this function must be called when entering the ISR.
* `postFromInterrupt()`: Like `post()` but from inside an ISR.
* `finalizePostFromInterrupt()`: This function must be called last in the ISR whether you called `postFromInterrupt()` or not.

### Mutex

Mutexes protect code sections from being accessed concurrently by multiple tasks. One task *locks* the mutex, so that another task has to wait on the mutex for the first task to *unlock* it. That's not busy waiting: The scheduler kicks in and resumes another task, most probably the one who is locking the mutex, because FreeRTOS supports [priority inheritance](https://www.freertos.org/Real-time-embedded-RTOS-mutexes.html). When the first task unlocks the mutex, other tasks waiting on it can proceed.

Normally you would protect variable accesses and keep the locked times short. But they can as well be used to guard an action that should not be interrupted or a resource that has to finish something before something new is started. Keep an eye on the locking sequence when multiple mutexes are involved: It's easy to shoot oneself in the foot and provoke a [deadlock](https://en.wikipedia.org/wiki/Deadlock). To avoid that either go for broader locking with fewer mutexes, or avoid nested locking by restructuring the code.

Mutexes in FreeRTOS can't be used from ISRs.

A `frt::Mutex` has a dead simple interface:
* `lock()`: Locks the mutex.
* `unlock()`: Unlocks the mutex.

### Semaphore

Semaphores synchronize actions like, "Proceed only when I told you so!" Thus, semaphores are "locked" in pristine state, whereas mutexes are unlocked. Mutexes must be "given back" via `unlock()`, whereas semaphores are "consumed". Usually there's one task `wait()`ing on a semaphore and another one `post()`ing on it. The normal (*counting*) semaphores remember how often they were posted so that the waiting task can proceed exactly that many times without blocking. Binary semaphores only remember if they were posted but not how often.

If you want to share data via a buffer (and don't want to use `frt::Queue`), you need a mutex to protect the buffer and a semaphore to notify the consumer. The mutex can only be ommitted when an ISR is involved, as interrupts are disabled then.
