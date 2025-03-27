#include <stdio.h>
#include "coroutine.h" // 假设这是你的协程库头文件

// Generator 协程函数
static void generator_func(void *arg) {
  for (int i = 0; i < 10; i++) {
    printf("生成器产生值: %d\n", i);
    co_yield();  // 暂停并返回控制权
  }
}

int main() {
  freopen("test.out", "w", stdout);
  coroutine_t *gen = co_start("generator", generator_func, NULL);
  for (int i = 0; i < 12; i++) {
    printf("消费者请求值 (第%d次)\n", i + 1);
    co_resume(gen);  // 恢复生成器协程
  }
  co_free(gen);
  return 0;
}