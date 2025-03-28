# Coroutine In C

A toy stackful coroutine implemented in C.

## `coroutine_t`
This is the core data structure representing a coroutine. It holds information about the coroutine's state, its function, stack, and context. The internal implementation of `coroutine_t` is not exposed to the user.

**Coroutine States**: Each coroutine can have one of the following states:
- `CO_NEW`: The coroutine is newly created and has not yet been scheduled for execution.
- `CO_RUNNING`: The coroutine is currently executing.
- `CO_WAITING`: The coroutine is waiting for another coroutine to finish.
- `CO_DEAD`: The coroutine has finished execution and is no longer active.

## API

### `co_start`

```c
coroutine_t *co_start(const char *name, void (*func)(void *), void *arg);
```

- **Description**: This function creates a new coroutine. The coroutine is initialized with a given name, a function to execute (`func`), and an argument to pass to the function (`arg`).
- **Parameters**:
    - `name`: The name of the coroutine.
    - `func`: The function that the coroutine will execute.
    - `arg`: The argument that will be passed to the function.
- **Returns**: A pointer to the created coroutine.
- **Example**:
  ```c
  coroutine_t *my_coroutine = co_start("my_coroutine", my_function, my_argument);
  ```

### `co_yield`

```c
void co_yield();
```

- **Description**: This function causes the current running coroutine to yield control, allowing other coroutines to run. The current coroutine is pushed to the back of the ready list, and the scheduler will choose the next coroutine to run.
- **Usage**: Call this function within a coroutine to voluntarily give up control and allow another coroutine to execute.
- **Example**:
  ```c
  co_yield();  // Yield control to another coroutine
  ```

### `co_wait`

```c
void co_wait(coroutine_t *co);
```

- **Description**: This function causes the current running coroutine to wait for another coroutine (`co`) to finish. The current coroutine is moved to the waiting list and will be resumed once the specified coroutine (`co`) reaches the "dead" state.
- **Parameters**:
    - `co`: The coroutine to wait for.
- **Usage**: Call this function when one coroutine needs to wait for the completion of another.
- **Example**:
  ```c
  co_wait(another_coroutine);  // Wait for 'another_coroutine' to finish
  ```

### `co_resume`

```c
void co_resume(coroutine_t *co);
```

- **Description**: This function resumes a coroutine (`co`) that has finished waiting. Resuming a coroutine that is already dead will have no effect. Resuming a coroutine of status `CO_WAITING` is not allowed.
- **Parameters**:
    - `co`: The coroutine to resume.
- **Usage**: Call this function to resume a coroutine that is currently pending to be executed.
- **Example**:
  ```c
  co_resume(another_coroutine);  // Resume 'another_coroutine'
  ```

### `co_free`

```c
void co_free(coroutine_t *co);
```

- **Description**: This function frees the resources associated with a coroutine (`co`) that has finished executing. The coroutine must be in the "dead" state before it can be freed.
- **Parameters**:
    - `co`: The coroutine to free.
- **Usage**: Call this function to free the memory allocated for a coroutine once it has completed its execution.
- **Example**:
  ```c
  co_free(another_coroutine);  // Free 'another_coroutine' after it finishes
  ```

## Notes

- **Stack Management**: Each coroutine has its own stack (`COROUTINE_STACK_SIZE = 32KB`) that is used during its execution.

- **Memory Allocation**: The library uses dynamic memory allocation (`malloc`) for coroutines, stacks, and other internal structures. It is important to call `co_free` for each coroutine after it has finished to avoid memory leaks.

- **Concurrency**: The library uses cooperative multitasking, meaning that coroutines yield control only when `co_yield` is called.

## Example Usage

### Example 1: Basic Coroutine Creation and Yielding

```c
#include "coroutine.h"

void task1(void *arg) {
  printf("Task 1 started\n");
  co_yield();
  printf("Task 1 resumed\n");
}

void task2(void *arg) {
  printf("Task 2 started\n");
  co_yield();
  printf("Task 2 resumed\n");
}

int main() {
  coroutine_t *co1 = co_start("Task 1", task1, NULL);
  coroutine_t *co2 = co_start("Task 2", task2, NULL);

  co_resume(co1);
  co_resume(co2);
  co_resume(co1);
  co_resume(co2);

  co_free(co1);
  co_free(co2);

  return 0;
}
```

**Output:**
```
Task 1 started
Task 2 started
Task 1 resumed
Task 2 resumed
```

This example shows how two coroutines (`task1` and `task2`) are created, started, and then resumed after yielding. The `co_yield` function allows the two tasks to run cooperatively.