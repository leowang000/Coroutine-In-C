#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "coroutine.h"

typedef struct {
  int n;
  unsigned long long result;
} FibTask;

// 预计算的斐波那契数
unsigned long long expected_fib(int n) {
  if (n <= 1) return n;

  unsigned long long a = 0, b = 1;
  for (int i = 2; i <= n; i++) {
    unsigned long long c = a + b;
    a = b;
    b = c;
  }
  return b;
}

void fibonacci(void *arg) {
  FibTask *task = (FibTask *) arg;

  if (task->n <= 1) {
    task->result = task->n;
    return;
  }

  // 分别计算fib(n-1)和fib(n-2)
  FibTask *task1 = malloc(sizeof(FibTask));
  FibTask *task2 = malloc(sizeof(FibTask));

  task1->n = task->n - 1;
  task2->n = task->n - 2;

  // 启动两个子协程
  coroutine_t *co1 = co_start("fib-1", fibonacci, task1);
  coroutine_t *co2 = co_start("fib-2", fibonacci, task2);

  // 等待两个子协程完成
  co_wait(co1);
  co_wait(co2);

  // 合并结果
  task->result = task1->result + task2->result;

  // 释放子任务
  free(task1);
  free(task2);
}

int main() {
  printf("斐波那契并行计算测试开始\n");

  // 测试几个不同的斐波那契数
  int test_values[] = {5, 10, 15};
  int num_tests = sizeof(test_values) / sizeof(test_values[0]);

  for (int i = 0; i < num_tests; i++) {
    int n = test_values[i];
    FibTask task = {n, 0};

    printf("计算斐波那契数 F(%d)...\n", n);

    coroutine_t *co = co_start("fib-root", fibonacci, &task);
    co_wait(co);

    unsigned long long expected = expected_fib(n);
    printf("F(%d) = %llu (期望值: %llu)\n", n, task.result, expected);

    assert(task.result == expected);
  }

  printf("斐波那契并行计算测试通过\n");
  return 0;
}