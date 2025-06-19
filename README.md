# C Coroutine Library

## Overview

This is a lightweight coroutine library implemented in C, designed to provide cooperative multitasking capabilities. The library features:

- **M:N scheduling model** - Coroutines (goroutines) are multiplexed onto a pool of OS threads
- **Work-stealing scheduler** - Efficient load balancing between processors
- **Semaphore support** - For synchronization between coroutines
- **Automatic cleanup** - Built-in destructor handles resource cleanup

## Features

- Create and manage lightweight coroutines
- Cooperative yielding between coroutines
- Coroutine waiting/joining
- Semaphore-based synchronization
- Multi-processor load balancing
- Automatic resource cleanup

## API Reference

### Coroutine Management

```c
// Create a new coroutine
coroutine_t co_start(const char *name, void (*func)(void *), void *arg);

// Yield execution to another coroutine
void co_yield();

// Wait for a coroutine to complete
void co_wait(coroutine_t g);
```

### Semaphore Operations

```c
// Create a semaphore with initial count
semaphore_t sem_create(int cnt);

// Increment semaphore (V operation)
void sem_up(semaphore_t sem);

// Decrement semaphore (P operation)
void sem_down(semaphore_t sem);

// Destroy a semaphore
void sem_destroy(semaphore_t sem);
```

## Usage Example

```c
#include <stdio.h>
#include "coroutine.h"

static int counter = 0;
static semaphore_t sem;

void counter_task(void *arg) {
  int id = *(int *)arg;

  for (int i = 0; i < 10; i++) {
    sem_down(sem);
    printf("Coroutine %d: counter = %d\n", id, counter);
    counter++;
    sem_up(sem);
    co_yield();
  }

  printf("Coroutine %d completed\n", id);
}

int main() {
  sem = sem_create(1);  // Create mutex semaphore

  int id1 = 1, id2 = 2;
  coroutine_t co1 = co_start("counter-1", counter_task, &id1);
  coroutine_t co2 = co_start("counter-2", counter_task, &id2);

  co_wait(co1);
  co_wait(co2);

  sem_destroy(sem);
  printf("Final count: %d\n", counter);

  return 0;
}
```

## Configuration Constants

The library can be configured using these constants in `coroutine.c`:

| Constant               | Default Value | Description                          |
|------------------------|---------------|--------------------------------------|
| `GOROUTINE_STACK_SIZE` | 32KB          | Stack size for each coroutine        |
| `RUNTIME_STACK_SIZE`   | 4KB           | Runtime stack size                   |
| `LOCAL_QUEUE_CAPACITY` | 256           | Processor-local queue capacity       |
| `MAX_GOROUTINES`       | 15000         | Maximum concurrent coroutines        |
| `LOGICAL_CORE_CNT`     | 32            | Logical core count                   |
| `MAX_PROCESSORS`       | 31            | Maximum processor threads (cores-1)  |

## Limitations

- Coroutines cannot be canceled once started
- No built-in channel or message passing mechanism
- Stack sizes are fixed at creation time
- Limited to POSIX-compatible systems