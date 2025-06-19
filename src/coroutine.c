#undef NDEBUG

#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coroutine.h"

#ifdef NDEBUG
#define debug(fmt, ...)
#else
#define debug(fmt, ...) fprintf(stderr, "\033[90m[debug] " fmt "\033[0m", ##__VA_ARGS__)
#endif

#define panic(fmt, ...) do { \
    fprintf(stderr, "\033[31mPANIC\033[0m at %s:%d in %s: " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define GOROUTINE_STACK_SIZE (32 * 1024) // 32KB
#define RUNTIME_STACK_SIZE (4 * 1024)    // 4KB
#define LOCAL_QUEUE_CAPACITY 256
#define MAX_GOROUTINES 15000
#define LOGICAL_CORE_CNT 32
#define MAX_PROCESSORS (LOGICAL_CORE_CNT - 1)

typedef struct WaiterListNode WaiterListNode;
typedef struct LocalQueue LocalQueue;
typedef struct GlobalQueueNode GlobalQueueNode;
typedef struct GlobalQueue GlobalQueue;
typedef struct Goroutine Goroutine;
typedef struct Processor Processor;
typedef struct Machine Machine;
typedef struct DeadListNode DeadListNode;
typedef struct DeadList DeadList;
typedef struct Scheduler Scheduler;

static WaiterListNode *waiter_list_node_create(WaiterListNode *next, Goroutine *g);

static void local_queue_init(LocalQueue *local_queue);
static void local_queue_cleanup(LocalQueue *local_queue);
static bool local_queue_push(LocalQueue *local_queue, Goroutine *g);
static Goroutine *local_queue_pop(LocalQueue *local_queue);
static Goroutine *local_queue_pop_back(LocalQueue *local_queue);
static int local_queue_size(LocalQueue *local_queue);
static Goroutine *local_queue_front(LocalQueue *local_queue);

static GlobalQueueNode *global_queue_node_create(GlobalQueueNode *next, Goroutine *g);
static void global_queue_node_destroy(GlobalQueueNode *global_queue_node);

static GlobalQueue *global_queue_create();
static void global_queue_destroy(GlobalQueue *global_queue);
static void global_queue_push(GlobalQueue *global_queue, Goroutine *g);
static void global_queue_push_no_lock(GlobalQueue *global_queue, Goroutine *g);
static Goroutine *global_queue_pop(GlobalQueue *global_queue);
static Goroutine *global_queue_pop_no_lock(GlobalQueue *global_queue);

static Goroutine *goroutine_create(const char *name, void (*func)(void *), void *arg);
static void goroutine_destroy(Goroutine *goroutine);

static void processor_init(Processor *processor, int id);
static void processor_cleanup(Processor *processor);

static void machine_init(Machine *machine, int id, Processor *p);
static void machine_cleanup(Machine *machine);

static DeadListNode *dead_list_node_create(DeadListNode *next, Goroutine *g);
static void dead_list_node_destroy(DeadListNode *dead_list_node);

static DeadList *dead_list_create();
static void dead_list_destroy(DeadList *dead_list);
static void dead_list_push(DeadList *dead_list, Goroutine *g);

static void scheduler_init();
static void scheduler_cleanup();

static Goroutine *current_g();
static Processor *current_p();
static Machine *current_m();
static void *g0_function(void *m);
static inline void stack_switch_call(void *sp, void *entry, void *arg);
static void schedule_to(Goroutine *g);
static void dead_handler(Goroutine *g);
static void g_wrapper(Goroutine *g);

struct WaiterListNode {
  WaiterListNode *next;
  Goroutine *g;
};

static WaiterListNode *waiter_list_node_create(WaiterListNode *next, Goroutine *g) {
  WaiterListNode *waiter_list_node = malloc(sizeof(WaiterListNode));
  if (waiter_list_node == NULL) {
    panic("Malloc for waiter_list_node fails.");
  }
  waiter_list_node->next = next;
  waiter_list_node->g = g;
  return waiter_list_node;
}

// Local queue has the ownership of the goroutines.
struct LocalQueue {
  Goroutine *queue[LOCAL_QUEUE_CAPACITY + 1];
  int head;
  int tail;
};

static void local_queue_init(LocalQueue *local_queue) {
  local_queue->head = 0;
  local_queue->tail = 0;
}

static void local_queue_cleanup(LocalQueue *local_queue) {
  if (local_queue == NULL) {
    return;
  }
  for (int i = local_queue->head; i != local_queue->tail; i = (i + 1) % (1 + LOCAL_QUEUE_CAPACITY)) {
    goroutine_destroy(local_queue->queue[i]);
  }
  local_queue->head = local_queue->tail;
}

static bool local_queue_push(LocalQueue *local_queue, Goroutine *g) {
  if ((local_queue->tail + 1) % (1 + LOCAL_QUEUE_CAPACITY) == local_queue->head) {
    return false;
  }
  local_queue->queue[local_queue->tail] = g;
  local_queue->tail = (local_queue->tail + 1) % (1 + LOCAL_QUEUE_CAPACITY);
  return true;
}

// The ownership of the popped goroutine is transferred out from the local_queue.
static Goroutine *local_queue_pop(LocalQueue *local_queue) {
  if (local_queue->tail == local_queue->head) {
    return NULL;
  }
  Goroutine *g = local_queue->queue[local_queue->head];
  local_queue->head = (local_queue->head + 1) % (1 + LOCAL_QUEUE_CAPACITY);
  return g;
}

// The ownership of the popped goroutine is transferred out from the local_queue.
static Goroutine *local_queue_pop_back(LocalQueue *local_queue) {
  if (local_queue->tail == local_queue->head) {
    return NULL;
  }
  int back = (local_queue->tail - 1 + (1 + LOCAL_QUEUE_CAPACITY)) % (1 + LOCAL_QUEUE_CAPACITY);
  Goroutine *g = local_queue->queue[back];
  local_queue->tail = back;
  return g;
}

static int local_queue_size(LocalQueue *local_queue) {
  return (local_queue->tail - local_queue->head + (1 + LOCAL_QUEUE_CAPACITY)) % (1 + LOCAL_QUEUE_CAPACITY);
}

static Goroutine *local_queue_front(LocalQueue *local_queue) {
  if (local_queue->head == local_queue->tail) {
    return NULL;
  }
  return local_queue->queue[local_queue->head];
}

struct GlobalQueueNode {
  GlobalQueueNode *next;
  Goroutine *g;
};

static GlobalQueueNode *global_queue_node_create(GlobalQueueNode *next, Goroutine *g) {
  GlobalQueueNode *global_queue_node = malloc(sizeof(GlobalQueueNode));
  if (global_queue_node == NULL) {
    panic("Malloc for global_queue_node fails.");
  }
  global_queue_node->next = next;
  global_queue_node->g = g;
  return global_queue_node;
}

static void global_queue_node_destroy(GlobalQueueNode *global_queue_node) {
  if (global_queue_node == NULL) {
    return;
  }
  goroutine_destroy(global_queue_node->g);
  free(global_queue_node);
}

struct GlobalQueue {
  GlobalQueueNode *head;
  GlobalQueueNode *tail;
  int size;
  pthread_mutex_t lock;
  pthread_cond_t not_empty_cond;
};

static GlobalQueue *global_queue_create() {
  GlobalQueue *global_queue = malloc(sizeof(GlobalQueue));
  if (global_queue == NULL) {
    panic("Malloc for global_queue fails.");
  }
  if (pthread_mutex_init(&global_queue->lock, NULL) != 0) {
    free(global_queue);
    panic("Error occurs when initializing global_queue->lock.");
  }
  if (pthread_cond_init(&global_queue->not_empty_cond, NULL) != 0) {
    free(global_queue);
    panic("Error occurs when initializing global_queue->not_empty_cond.");
  }
  global_queue->head = NULL;
  global_queue->tail = NULL;
  global_queue->size = 0;
  return global_queue;
}

static void global_queue_destroy(GlobalQueue *global_queue) {
  if (global_queue == NULL) {
    return;
  }
  pthread_mutex_lock(&global_queue->lock);
  GlobalQueueNode *cur = global_queue->head;
  while (cur != NULL) {
    GlobalQueueNode *next = cur->next;
    global_queue_node_destroy(cur);
    cur = next;
  }
  pthread_mutex_unlock(&global_queue->lock);
  pthread_mutex_destroy(&global_queue->lock);
  pthread_cond_destroy(&global_queue->not_empty_cond);
  free(global_queue);
}

static void global_queue_push(GlobalQueue *global_queue, Goroutine *g) {
  pthread_mutex_lock(&global_queue->lock);
  global_queue_push_no_lock(global_queue, g);
  pthread_mutex_unlock(&global_queue->lock);
}

static void global_queue_push_no_lock(GlobalQueue *global_queue, Goroutine *g) {
  GlobalQueueNode *node = global_queue_node_create(NULL, g);
  if (global_queue->size == 0) {
    global_queue->head = node;
    global_queue->tail = node;
  } else {
    global_queue->tail->next = node;
    global_queue->tail = node;
  }
  global_queue->size++;
  pthread_cond_signal(&global_queue->not_empty_cond);
}

// The popped queue node is freed, but the ownership of its inner goroutine is moved out from the global queue.
static Goroutine *global_queue_pop(GlobalQueue *global_queue) {
  pthread_mutex_lock(&global_queue->lock);
  Goroutine *g = global_queue_pop_no_lock(global_queue);
  pthread_mutex_unlock(&global_queue->lock);
  return g;
}

static Goroutine *global_queue_pop_no_lock(GlobalQueue *global_queue) {
  if (global_queue->size == 0) {
    return NULL;
  }
  GlobalQueueNode *node = global_queue->head;
  Goroutine *g = node->g;
  global_queue->head = global_queue->head->next;
  global_queue->size--;
  if (global_queue->size == 0) {
    global_queue->tail = NULL;
  } else {
    pthread_cond_signal(&global_queue->not_empty_cond);
  }
  free(node);
  return g;
}

struct Goroutine {
  /* identification */
  char *name;
  /* the task */
  void (*func)(void *);
  void *arg;
  /* execution state */
  enum {
    GOROUTINE_NEW,
    GOROUTINE_RUNNING,
    GOROUTINE_WAITING,
    GOROUTINE_DEAD
  } status;
  jmp_buf context;
  uint8_t *stack;
  WaiterListNode *waiter_list_head;
  // Ensure that all state changes are atomic.
  pthread_mutex_t status_lock;
  /* scheduler information */
};

static Goroutine *goroutine_create(const char *name, void (*func)(void *), void *arg) {
  Goroutine *goroutine = malloc(sizeof(Goroutine));
  if (goroutine == NULL) {
    panic("Malloc for goroutine fails.");
  }
  goroutine->name = malloc(strlen(name) + 1);
  if (goroutine->name == NULL) {
    panic("Malloc for goroutine->name fails.");
  }
  strcpy(goroutine->name, name);
  goroutine->func = func;
  goroutine->arg = arg;
  goroutine->status = GOROUTINE_NEW;
  goroutine->stack = NULL;
  goroutine->waiter_list_head = NULL;
  pthread_mutex_init(&goroutine->status_lock, NULL);
  return goroutine;
}

static void goroutine_destroy(Goroutine *goroutine) {
  if (goroutine == NULL) {
    return;
  }
  free(goroutine->name);
  assert(goroutine->waiter_list_head == NULL);
  pthread_mutex_destroy(&goroutine->status_lock);
  free(goroutine);
}

struct Processor {
  /* identification */
  int id;
  /* goroutines */
  LocalQueue local_queue;
  uint8_t *runtime_stack;
  /* scheduler information */
};

static void processor_init(Processor *processor, int id) {
  processor->id = id;
  local_queue_init(&processor->local_queue);
  processor->runtime_stack = malloc(RUNTIME_STACK_SIZE);
  if (processor->runtime_stack == NULL) {
    panic("Malloc for processor->runtime_stack fails.");
  }
}

static void processor_cleanup(Processor *processor) {
  local_queue_cleanup(&processor->local_queue);
  free(processor->runtime_stack);
}

struct Machine {
  /* identification */
  int id;
  /* scheduling context */
  Goroutine *g0; // the scheduling goroutine for the machine (i.e. the "idle task")
  Goroutine *cur_g; // non-owning reference
  /* scheduler information */
  Processor *p; // non-owning reference
  /* tmp */
  Goroutine *g_waiting; // non-owning reference
};

static void machine_init(Machine *machine, int id, Processor *p) {
  machine->id = id;
  static char name_buffer[50];
  snprintf(name_buffer, sizeof(name_buffer), "machine-%d g0", id);
  machine->g0 = goroutine_create(name_buffer, NULL, NULL);
  machine->g0->status = GOROUTINE_RUNNING;
  machine->cur_g = machine->g0;
  machine->p = p;
  machine->g_waiting = NULL;
}

static void machine_cleanup(Machine *machine) {
  goroutine_destroy(machine->g0);
}

struct DeadListNode {
  DeadListNode *next;
  Goroutine *g;
};

static DeadListNode *dead_list_node_create(DeadListNode *next, Goroutine *g) {
  DeadListNode *dead_list_node = malloc(sizeof(DeadListNode));
  if (dead_list_node == NULL) {
    panic("Malloc for dead_list_node fails.");
  }
  dead_list_node->next = next;
  dead_list_node->g = g;
  return dead_list_node;
}

static void dead_list_node_destroy(DeadListNode *dead_list_node) {
  if (dead_list_node == NULL) {
    return;
  }
  goroutine_destroy(dead_list_node->g);
  free(dead_list_node);
}

struct DeadList {
  DeadListNode *head;
  pthread_mutex_t lock;
};

static DeadList *dead_list_create() {
  DeadList *dead_list = malloc(sizeof(DeadList));
  if (dead_list == NULL) {
    panic("Malloc for dead_list fails.");
  }
  if (pthread_mutex_init(&dead_list->lock, NULL) != 0) {
    free(dead_list);
    panic("Error occurs when initializing dead_list->lock.");
  }
  dead_list->head = NULL;
  return dead_list;
}

static void dead_list_destroy(DeadList *dead_list) {
  if (dead_list == NULL) {
    return;
  }
  pthread_mutex_lock(&dead_list->lock);
  DeadListNode *cur = dead_list->head;
  while (cur != NULL) {
    DeadListNode *next = cur->next;
    dead_list_node_destroy(cur);
    cur = next;
  }
  pthread_mutex_unlock(&dead_list->lock);
  pthread_mutex_destroy(&dead_list->lock);
  free(dead_list);
}

static void dead_list_push(DeadList *dead_list, Goroutine *g) {
  pthread_mutex_lock(&dead_list->lock);
  DeadListNode *dead_list_node = dead_list_node_create(dead_list->head, g);
  dead_list->head = dead_list_node;
  pthread_mutex_unlock(&dead_list->lock);
}

struct Scheduler {
  /* scheduler components */
  Machine m_main;
  Processor p[MAX_PROCESSORS];
  Machine m[MAX_PROCESSORS];
  pthread_t threads[MAX_PROCESSORS];
  GlobalQueue *global_queue;
  DeadList *dead_list;
  /* scheduler status */
  _Atomic enum {
    SCHEDULER_INIT = 0, SCHEDULER_RUNNING, SCHEDULER_STOPPED
  } status;
  _Atomic int active_g_cnt;
  bool main_waiting;
  pthread_mutex_t main_wait_lock;
  pthread_cond_t main_wait_cond;
  pthread_key_t key;
};

Scheduler g_scheduler;

static void scheduler_init() {
  machine_init(&g_scheduler.m_main, 0, NULL);
  g_scheduler.global_queue = global_queue_create();
  g_scheduler.dead_list = dead_list_create();
  atomic_store_explicit(&g_scheduler.status, SCHEDULER_RUNNING, memory_order_release);
  atomic_store_explicit(&g_scheduler.active_g_cnt, 0, memory_order_release);
  g_scheduler.main_waiting = false;
  if (pthread_mutex_init(&g_scheduler.main_wait_lock, NULL) != 0) {
    panic("Error occurs when initializing g_scheduler.main_wait_lock");
  }
  if (pthread_cond_init(&g_scheduler.main_wait_cond, NULL) != 0) {
    panic("Error occurs when initializing g_scheduler.main_wait_cond");
  }
  if (pthread_key_create(&g_scheduler.key, NULL) != 0) {
    panic("Error occurs when initializing g_scheduler.key");
  }
  if (pthread_setspecific(g_scheduler.key, &g_scheduler.m_main) != 0) {
    panic("Error occurs when setting key for thread-0");
  }
  for (int i = 0; i < MAX_PROCESSORS; i++) {
    processor_init(&g_scheduler.p[i], i + 1);
    machine_init(&g_scheduler.m[i], i + 1, &g_scheduler.p[i]);
    if (pthread_create(&g_scheduler.threads[i], NULL, g0_function, &g_scheduler.m[i]) != 0) {
      panic("Error occurs when creating thread-%d.", i + 1);
    }
  }
}

__attribute__ ((destructor))
static void scheduler_cleanup() {
  if (atomic_load_explicit(&g_scheduler.active_g_cnt, memory_order_acquire) > 0) {
    panic("Some goroutines are still running.");
  }
  atomic_store_explicit(&g_scheduler.status, SCHEDULER_STOPPED, memory_order_release);
  // wake up all threads
  pthread_mutex_lock(&g_scheduler.global_queue->lock);
  pthread_cond_broadcast(&g_scheduler.global_queue->not_empty_cond);
  pthread_mutex_unlock(&g_scheduler.global_queue->lock);
  for (int i = 0; i < MAX_PROCESSORS; i++) {
    if (pthread_join(g_scheduler.threads[i], NULL) != 0) {
      panic("Error occurs when joining thread-%d", i + 1);
    }
  }
  machine_cleanup(&g_scheduler.m_main);
  for (int i = 0; i < MAX_PROCESSORS; i++) {
    processor_cleanup(&g_scheduler.p[i]);
    machine_cleanup(&g_scheduler.m[i]);
  }
  global_queue_destroy(g_scheduler.global_queue);
  dead_list_destroy(g_scheduler.dead_list);
  pthread_mutex_destroy(&g_scheduler.main_wait_lock);
  pthread_cond_destroy(&g_scheduler.main_wait_cond);
  pthread_key_delete(g_scheduler.key);
}

static Goroutine *current_g() {
  Machine *value = pthread_getspecific(g_scheduler.key);
  if (value == NULL) {
    panic("Error occurs when getting key in current_g");
  }
  return value->cur_g;
}

static Processor *current_p() {
  Machine *value = pthread_getspecific(g_scheduler.key);
  if (value == NULL) {
    panic("Error occurs when getting key in current_p");
  }
  return value->p;
}

static Machine *current_m() {
  Machine *value = pthread_getspecific(g_scheduler.key);
  if (value == NULL) {
    panic("Error occurs when getting key in current_m");
  }
  return value;
}

void *g0_function(void *m) {
  Machine *machine = (Machine *) m;
  if (pthread_setspecific(g_scheduler.key, machine) != 0) {
    panic("Error occurs when setting key for thread-%d", machine->id);
  }
  while (atomic_load_explicit(&g_scheduler.status, memory_order_acquire) == SCHEDULER_RUNNING) {
    int avg = atomic_load_explicit(&g_scheduler.active_g_cnt, memory_order_acquire) / MAX_PROCESSORS;
    int target = (avg + 1 > LOCAL_QUEUE_CAPACITY ? LOCAL_QUEUE_CAPACITY : avg + 1);
    if (local_queue_size(&machine->p->local_queue) <= target / 2) {
      pthread_mutex_lock(&g_scheduler.global_queue->lock);
      while (local_queue_size(&machine->p->local_queue) < target) {
        Goroutine *new_g = global_queue_pop_no_lock(g_scheduler.global_queue);
        if (new_g == NULL) {
          break;
        }
        local_queue_push(&machine->p->local_queue, new_g);
      }
      pthread_mutex_unlock(&g_scheduler.global_queue->lock);
    }
    if (local_queue_size(&machine->p->local_queue) == 0) {
      pthread_mutex_lock(&g_scheduler.global_queue->lock);
      while (g_scheduler.global_queue->size == 0 &&
             atomic_load_explicit(&g_scheduler.status, memory_order_acquire) == SCHEDULER_RUNNING) {
        pthread_cond_wait(&g_scheduler.global_queue->not_empty_cond, &g_scheduler.global_queue->lock);
      }
      if (atomic_load_explicit(&g_scheduler.status, memory_order_acquire) != SCHEDULER_RUNNING) {
        // The current thread is woken up by the main thread in the destructor.
        pthread_mutex_unlock(&g_scheduler.global_queue->lock);
        break;
      }
      Goroutine *new_g = global_queue_pop_no_lock(g_scheduler.global_queue);
      pthread_mutex_unlock(&g_scheduler.global_queue->lock);
      assert(new_g != NULL);
      local_queue_push(&machine->p->local_queue, new_g);
    }
    if (setjmp(machine->g0->context) == 0) {
      schedule_to(local_queue_front(&machine->p->local_queue));
    }
    if (machine->g_waiting != NULL) {
      // Scheduled back from co_wait.
      pthread_mutex_unlock(&machine->g_waiting->status_lock);
      machine->g_waiting = NULL;
    }
  }
  return NULL;
}

// Jump to the function at entry, with one argument arg.
// Switch to a new stack, with sp as its stack top.
static inline void stack_switch_call(void *sp, void *entry, void *arg) {
  asm volatile (
#if __x86_64__
      "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
      :
      : "b"((uintptr_t) sp - 8), "d"(entry), "a"(arg)
      : "memory"
#else
    "movl %0, %%esp; movl %2, 4(%0); jmp *%1"
    :
    : "b"((uintptr_t) sp - 8), "d"(entry), "a"(arg)
    : "memory"
#endif
      );
}

static void schedule_to(Goroutine *g) {
  current_m()->cur_g = g;
  // We always schedule within the same M, so there is no need to lock.
  switch (g->status) {
    case GOROUTINE_NEW:
      g->stack = malloc(GOROUTINE_STACK_SIZE);
      if (g->stack == NULL) {
        panic("Malloc for g->stack fails.");
      }
      stack_switch_call(g->stack + GOROUTINE_STACK_SIZE, g_wrapper, g);
      break;
    case GOROUTINE_RUNNING:
      longjmp(g->context, 1);
    case GOROUTINE_WAITING:
    case GOROUTINE_DEAD:
      assert(false);
  }
}

static void dead_handler(Goroutine *g) {
  free(g->stack);
  g->stack = NULL;
  schedule_to(current_m()->g0);
}

static void g_wrapper(Goroutine *g) {
  g->func(g->arg);
  assert(local_queue_pop(&current_p()->local_queue) == g);
  pthread_mutex_lock(&g->status_lock);
  g->status = GOROUTINE_DEAD;
  atomic_fetch_sub_explicit(&g_scheduler.active_g_cnt, 1, memory_order_release);
  pthread_mutex_lock(&g_scheduler.global_queue->lock);
  for (WaiterListNode *waiter = g->waiter_list_head; waiter != NULL; waiter = waiter->next) {
    pthread_mutex_lock(&waiter->g->status_lock);
    waiter->g->status = GOROUTINE_RUNNING;
    if (waiter->g == g_scheduler.m_main.g0) {
      pthread_mutex_lock(&g_scheduler.main_wait_lock);
      g_scheduler.main_waiting = false;
      pthread_cond_signal(&g_scheduler.main_wait_cond);
      pthread_mutex_unlock(&g_scheduler.main_wait_lock);
    } else {
      global_queue_push_no_lock(g_scheduler.global_queue, waiter->g);
    }
    pthread_mutex_unlock(&waiter->g->status_lock);
  }
  pthread_mutex_unlock(&g_scheduler.global_queue->lock);
  WaiterListNode *cur = g->waiter_list_head;
  while (cur != NULL) {
    WaiterListNode *next = cur->next;
    free(cur);
    cur = next;
  }
  g->waiter_list_head = NULL;
  pthread_mutex_unlock(&g->status_lock);
  dead_list_push(g_scheduler.dead_list, g);
  stack_switch_call(current_p()->runtime_stack + RUNTIME_STACK_SIZE, dead_handler, g);
}

Goroutine *co_start(const char *name, void (*func)(void *), void *arg) {
  if (atomic_load_explicit(&g_scheduler.status, memory_order_acquire) == SCHEDULER_INIT) {
    scheduler_init();
  }
  int active = atomic_load_explicit(&g_scheduler.active_g_cnt, memory_order_acquire);
  if (active >= MAX_GOROUTINES) {
    panic("Too much goroutines.");
  }
  Goroutine *g = goroutine_create(name, func, arg);
  if (current_m() == &g_scheduler.m_main) {
    global_queue_push(g_scheduler.global_queue, g);
  } else {
    LocalQueue *local_queue = &current_p()->local_queue;
    int avg = active / MAX_PROCESSORS;
    int target = (avg + 1 > LOCAL_QUEUE_CAPACITY ? LOCAL_QUEUE_CAPACITY : avg + 1);
    if (local_queue_size(local_queue) + 1 >= 1.5 * target) {
      pthread_mutex_lock(&g_scheduler.global_queue->lock);
      global_queue_push_no_lock(g_scheduler.global_queue, g);
      while (local_queue_size(local_queue) > target) {
        global_queue_push_no_lock(g_scheduler.global_queue, local_queue_pop_back(local_queue));
      }
      pthread_mutex_unlock(&g_scheduler.global_queue->lock);
    } else if (!local_queue_push(local_queue, g)) {
      global_queue_push(g_scheduler.global_queue, g);
    }
  }
  atomic_fetch_add_explicit(&g_scheduler.active_g_cnt, 1, memory_order_release);
  return g;
}

void co_yield() {
  if (atomic_load_explicit(&g_scheduler.status, memory_order_acquire) == SCHEDULER_INIT) {
    scheduler_init();
  }
  Machine *cur_m = current_m();
  if (cur_m == &g_scheduler.m_main) {
    return;
  }
  Goroutine *cur_g = cur_m->cur_g;
  assert(local_queue_pop(&current_p()->local_queue) == cur_g);
  local_queue_push(&current_p()->local_queue, cur_g);
  pthread_mutex_lock(&cur_g->status_lock);
  assert(cur_g->status == GOROUTINE_NEW || cur_g->status == GOROUTINE_RUNNING);
  cur_g->status = GOROUTINE_RUNNING;
  pthread_mutex_unlock(&cur_g->status_lock);
  if (setjmp(cur_g->context) == 0) {
    schedule_to(cur_m->g0);
  }
}

void co_wait(Goroutine *goroutine) {
  if (atomic_load_explicit(&g_scheduler.status, memory_order_acquire) == SCHEDULER_INIT) {
    panic("No coroutine to wait.");
  }
  Machine *cur_m = current_m();
  if (cur_m == &g_scheduler.m_main) {
    pthread_mutex_lock(&goroutine->status_lock);
    if (goroutine->status == GOROUTINE_DEAD) {
      pthread_mutex_unlock(&goroutine->status_lock);
    } else {
      Goroutine *cur_g = current_g();
      pthread_mutex_lock(&cur_g->status_lock);
      cur_g->status = GOROUTINE_WAITING;
      pthread_mutex_unlock(&cur_g->status_lock);
      g_scheduler.main_waiting = true;
      goroutine->waiter_list_head = waiter_list_node_create(goroutine->waiter_list_head, cur_g);
      pthread_mutex_unlock(&goroutine->status_lock);
      // goroutine may exit between these two lines, and main_waiting is set to false, but it doesn't matter.
      pthread_mutex_lock(&g_scheduler.main_wait_lock);
      while (g_scheduler.main_waiting) {
        pthread_cond_wait(&g_scheduler.main_wait_cond, &g_scheduler.main_wait_lock);
      }
      pthread_mutex_unlock(&g_scheduler.main_wait_lock);
    }
    return;
  } else {
    pthread_mutex_lock(&goroutine->status_lock);
    if (goroutine->status == GOROUTINE_DEAD) {
      pthread_mutex_unlock(&goroutine->status_lock);
    } else {
      Goroutine *cur_g = current_g();
      assert(local_queue_pop(&current_p()->local_queue) == cur_g);
      // If circular co_wait exists, deadlock may occur.
      pthread_mutex_lock(&cur_g->status_lock);
      cur_g->status = GOROUTINE_WAITING;
      pthread_mutex_unlock(&cur_g->status_lock);
      goroutine->waiter_list_head = waiter_list_node_create(goroutine->waiter_list_head, cur_g);
      // We don't unlock goroutine->status_lock here. We will unlock it when we schedule back to g0.
      cur_m->g_waiting = goroutine;
      if (setjmp(cur_g->context) == 0) {
        schedule_to(cur_m->g0);
      }
    }
  }
}
