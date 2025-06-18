#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "coroutine.h"

#define CHAIN_LENGTH 10000

typedef struct {
  int value;
  int processed;
} ChainNode;

void chain_processor(void *arg) {
  ChainNode *node = (ChainNode*)arg;

  // 模拟一些工作
  for (int i = 0; i < 1000; i++) {
    if (i % 200 == 0) co_yield();
  }

  // 标记为已处理
  node->processed = 1;

  // 增加节点值
  node->value += 10;
}

int main() {
  printf("协程依赖链测试开始\n");

  ChainNode nodes[CHAIN_LENGTH];
  coroutine_t *coroutines[CHAIN_LENGTH];

  // 初始化节点
  for (int i = 0; i < CHAIN_LENGTH; i++) {
    nodes[i].value = i;
    nodes[i].processed = 0;
  }

  // 启动所有协程
  for (int i = 0; i < CHAIN_LENGTH; i++) {
    char name[32];
    sprintf(name, "chain-%d", i);
    coroutines[i] = co_start(name, chain_processor, &nodes[i]);
  }

  // 按照特定顺序等待协程，形成依赖链
  // 例如：先等待奇数索引，再等待偶数索引
  for (int i = 1; i < CHAIN_LENGTH; i += 2) {
    co_wait(coroutines[i]);
  }

  for (int i = 0; i < CHAIN_LENGTH; i += 2) {
    co_wait(coroutines[i]);
  }

  // 验证所有协程都已处理
  for (int i = 0; i < CHAIN_LENGTH; i++) {
    assert(nodes[i].processed == 1);
    assert(nodes[i].value == i + 10);
  }

  printf("协程依赖链测试通过\n");
  return 0;
}