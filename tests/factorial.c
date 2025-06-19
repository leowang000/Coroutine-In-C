#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "coroutine.h"

#define NUM_COROUTINES 10000
#define ITERATIONS 1000000

typedef struct {
  int id;
  unsigned long long result;
} ComputeTask;

void compute_factorial(void *arg) {
  ComputeTask *task = (ComputeTask *) arg;

  unsigned long long factorial = 1;
  // 计算小阶乘，避免溢出
  int n = (task->id % 10) + 1;

  for (int i = 0; i < ITERATIONS; i++) {
    factorial = 1;
    for (int j = 1; j <= n; j++) {
      factorial *= j;
    }

    if (i % 10000 == 0) {
      co_yield(); // 偶尔让出CPU
    }
  }

  task->result = factorial;
}

// 预计算的阶乘结果
unsigned long long expected_factorial(int n) {
  unsigned long long result = 1;
  for (int i = 1; i <= n; i++) {
    result *= i;
  }
  return result;
}

ComputeTask tasks[NUM_COROUTINES];
coroutine_t coroutines[NUM_COROUTINES];

void omain(void *) {
  printf("独立计算测试开始\n");
  // 创建多个执行独立计算的协程
  for (int i = 0; i < NUM_COROUTINES; i++) {
    tasks[i].id = i;
    tasks[i].result = 0;

    char name[32];
    sprintf(name, "compute-%d", i);
    coroutines[i] = co_start(name, compute_factorial, &tasks[i]);
  }

  // 等待所有计算完成
  for (int i = 0; i < NUM_COROUTINES; i++) {
    co_wait(coroutines[i]);
  }

  // 验证结果
  for (int i = 0; i < NUM_COROUTINES; i++) {
    int n = (i % 10) + 1;
    assert(tasks[i].result == expected_factorial(n));
  }

  printf("独立计算测试通过\n");
}

int main() {
  coroutine_t g_main = co_start("main", omain, NULL);
  co_wait(g_main);
  return 0;
}