#include <stdio.h>
#include <string.h>
#include "coroutine.h"

int g_count = 0;

static void add_count() {
  g_count++;
}

static int get_count() {
  return g_count;
}

static void work_loop(void *arg) {
  const char *s = (const char *) arg;
  for (int i = 0; i < 100; ++i) {
    printf("%s%d  ", s, get_count());
    add_count();
    co_yield();
  }
}

static void work(void *arg) {
  work_loop(arg);
}

static void test_1() {
  coroutine_t *thd1 = co_start("thread-1", work, "X");
  coroutine_t *thd2 = co_start("thread-2", work, "Y");
  co_wait(thd1);
  co_wait(thd2);
  co_free(thd1);
  co_free(thd2);
}

int main() {
  freopen("test.out", "w", stdout);
  setbuf(stdout, NULL);
  printf("Test #1. Expect: (X|Y){0, 1, 2, ..., 199}\n");
  test_1();
  return 0;
}