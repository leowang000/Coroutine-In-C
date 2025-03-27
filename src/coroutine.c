#undef NDEBUG

#include <assert.h>
#include <setjmp.h>
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

#define COROUTINE_STACK_SIZE (32 * 1024) // 32KB
#define RUNTIME_STACK_SIZE (4 * 1024)    // 4KB

uint8_t runtime_stack[RUNTIME_STACK_SIZE];

enum co_status {
  CO_NEW,
  CO_RUNNING,
  CO_WAITING,
  CO_DEAD
};

struct list;

typedef struct list_node waiter_list_node;

struct co {
  char *name;
  void (*func)(void *);
  void *arg;
  enum co_status status;
  jmp_buf context;
  uint8_t *stack;
  waiter_list_node *waiter_list_head;
  struct list_node *prev; // the position in ready_list/waiting_list/dead_list
};

struct co *current;

struct list_node {
  struct list_node *next;
  struct co *co;
};

struct list {
  int len;
  struct list_node *head;
  struct list_node *tail;
};

static void list_init_(struct list *list) {
  list->len = 0;
  list->head = malloc(sizeof(struct list_node));
  if (list->head == NULL) {
    panic("malloc for list->head fails");
  }
  list->tail = list->head;
}

static void list_insert_after_(struct list *list, struct list_node *pos, struct list_node *node) {
  list->len++;
  node->co->prev = pos;
  node->next = pos->next;
  if (pos->next == NULL) {
    list->tail = node;
  } else {
    pos->next->co->prev = node;
  }
  pos->next = node;
}

static struct list_node *list_erase_(struct list *list, struct list_node *node) {
  list->len--;
  node->co->prev->next = node->next;
  if (node->next == NULL) {
    list->tail = node->co->prev;
  } else {
    node->next->co->prev = node->co->prev;
  }
  return node;
}

static void list_push_back_(struct list *list, struct list_node *node) {
  list_insert_after_(list, list->tail, node);
}

static struct list_node *list_front_(struct list *list) {
  assert(list->len > 0);
  return list->head->next;
}

static void list_free_(struct list_node *list_head) {
  struct list_node *cur = list_head;
  while (cur != NULL) {
    struct list_node *next = cur->next;
    free(cur);
    cur = next;
  }
}

static inline struct list_node *belong_node_(const struct co *co) {
  return co->prev->next;
}

// ready_list, waiting_list, dead_list are exclusive. All coroutine must belong to one and only one of them.
struct list ready_list;   // status: CO_NEW/CO_RUNNING
struct list waiting_list; // status: CO_WAITING
struct list dead_list;    // status: CO_DEAD

static void initialize_();
static inline void stack_switch_call_(void *sp, void *entry, void *arg);
static void schedule_to_(struct co *co);
static void schedule_();
static void dead_handler_(struct co *co);
static void co_wrapper_(struct co *co);
static void cleanup_();

__attribute__ ((constructor))
static void initialize_() {
  list_init_(&ready_list);
  list_init_(&waiting_list);
  list_init_(&dead_list);
  current = co_start("main", NULL, NULL);
}

struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  struct co *co = malloc(sizeof(struct co));
  if (co == NULL) {
    panic("malloc for co fails");
  }
  co->name = malloc(strlen(name) + 1);
  if (co->name == NULL) {
    panic("malloc for co->name fails");
  }
  strcpy(co->name, name);
  co->func = func;
  co->arg = arg;
  co->status = CO_NEW;
  co->waiter_list_head = NULL;
  struct list_node *node = malloc(sizeof(struct list_node));
  if (node == NULL) {
    panic("malloc for node fails");
  }
  node->co = co;
  list_push_back_(&ready_list, node);
  return co;
}

static inline void stack_switch_call_(void *sp, void *entry, void *arg) {
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

static void schedule_to_(struct co *co) {
  current = co;
  switch (current->status) {
    case CO_NEW:
      current->stack = malloc(COROUTINE_STACK_SIZE);
      if (current->stack == NULL) {
        panic("malloc for co->stack fails");
      }
      stack_switch_call_(current->stack + COROUTINE_STACK_SIZE, co_wrapper_, current);
    case CO_RUNNING:
      longjmp(current->context, 1);
    case CO_WAITING:
    case CO_DEAD:
      assert(false);
  }
}

static void schedule_() {
  if (ready_list.len == 0) {
    panic("no coroutine to schedule");
  }
  schedule_to_(list_front_(&ready_list)->co);
}

static void dead_handler_(struct co *co) {
  free(co->stack);
  co->stack = NULL;
  schedule_();
}

static void co_wrapper_(struct co *co) {
  co->func(co->arg);
  co->status = CO_DEAD;
  list_push_back_(&dead_list, list_erase_(&ready_list, belong_node_(current)));
  for (waiter_list_node *waiter_node = co->waiter_list_head; waiter_node != NULL; waiter_node = waiter_node->next) {
    waiter_node->co->status = CO_RUNNING;
    list_push_back_(&ready_list, list_erase_(&waiting_list, belong_node_(waiter_node->co)));
  }
  list_free_(co->waiter_list_head);
  co->waiter_list_head = NULL;
  stack_switch_call_(runtime_stack + RUNTIME_STACK_SIZE, dead_handler_, co);
}

void co_yield() {
  assert(current->status == CO_NEW || current->status == CO_RUNNING);
  current->status = CO_RUNNING;
  list_push_back_(&ready_list, list_erase_(&ready_list, belong_node_(current))); // lower the priority of current
  if (setjmp(current->context) == 0) {
    schedule_();
  }
}

void co_wait(struct co *co) {
  assert(current->status == CO_NEW || current->status == CO_RUNNING);
  if (co->status == CO_DEAD) {
    return;
  }
  current->status = CO_WAITING;
  list_push_back_(&waiting_list, list_erase_(&ready_list, belong_node_(current)));
  waiter_list_node *waiter_node = malloc(sizeof(waiter_list_node));
  if (waiter_node == NULL) {
    panic("malloc for waiter_node fails");
  }
  waiter_node->co = current;
  waiter_node->next = co->waiter_list_head;
  co->waiter_list_head = waiter_node;
  if (setjmp(current->context) == 0) {
    schedule_();
  }
}

void co_resume(struct co *co) {
  assert(current->status == CO_NEW || current->status == CO_RUNNING);
  switch (co->status) {
    case CO_NEW:
    case CO_RUNNING:
      current->status = CO_RUNNING;
      if (setjmp(current->context) == 0) {
        schedule_to_(co);
      }
      break;
    case CO_WAITING:
      panic("resuming a coroutine of status CO_WAITING"); // TODO: cascading wait
    case CO_DEAD:
      break;
  }
}

void co_free(struct co *co) {
  assert(co != NULL);
  assert(co->status == CO_DEAD);
  free(co->name);
  list_free_(co->waiter_list_head);
  free(list_erase_(&dead_list, belong_node_(co)));
  free(co);
}

__attribute__((destructor))
static void cleanup_() {
  assert(strcmp(current->name, "main") == 0);
  assert(ready_list.len == 1);
  assert(waiting_list.len == 0);
  if (dead_list.len > 0) {
    panic("dead coroutines not freed");
  }
  free(current->name);
  list_free_(current->waiter_list_head);
  free(current);
  list_free_(ready_list.head);
  list_free_(waiting_list.head);
  list_free_(dead_list.head);
}
