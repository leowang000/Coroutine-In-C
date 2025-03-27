#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "producer-consumer.h"

int g_count = 200;

static int g_running = 1;

static void do_produce(Queue *queue) {
  assert(!q_is_full(queue));
  Item *item = (Item *) malloc(sizeof(Item));
  if (!item) {
    fprintf(stderr, "New item failure\n");
    return;
  }
  item->data = (char *) malloc(10);
  if (!item->data) {
    fprintf(stderr, "New data failure\n");
    free(item);
    return;
  }
  memset(item->data, 0, 10);
  sprintf(item->data, "libco-%d", g_count++);
  q_push(queue, item);
}

static void producer(void *arg) {
  Queue *queue = (Queue *) arg;
  for (int i = 0; i < 100;) {
    if (!q_is_full(queue)) {
      do_produce(queue);
      i += 1;
    }
    co_yield();
  }
}

static void do_consume(Queue *queue) {
  assert(!q_is_empty(queue));
  Item *item = q_pop(queue);
  if (item) {
    printf("%s  ", (char *) item->data);
    free(item->data);
    free(item);
  }
}

static void consumer(void *arg) {
  Queue *queue = (Queue *) arg;
  while (g_running) {
    if (!q_is_empty(queue)) {
      do_consume(queue);
    }
    co_yield();
  }
}

static void test_2() {

  Queue *queue = q_new();

  coroutine_t *thd1 = co_start("producer-1", producer, queue);
  coroutine_t *thd2 = co_start("producer-2", producer, queue);
  coroutine_t *thd3 = co_start("consumer-1", consumer, queue);
  coroutine_t *thd4 = co_start("consumer-2", consumer, queue);

  co_wait(thd1);
  co_wait(thd2);

  g_running = 0;

  co_wait(thd3);
  co_wait(thd4);

  while (!q_is_empty(queue)) {
    do_consume(queue);
  }

  q_free(queue);

  co_free(thd1);
  co_free(thd2);
  co_free(thd3);
  co_free(thd4);
}

int main() {
  freopen("test.out", "w", stdout);
  setbuf(stdout, NULL);
  printf("\n\nTest #2. Expect: (libco-){200, 201, 202, ..., 399}\n");
  test_2();
  printf("\n\n");
  return 0;
}
