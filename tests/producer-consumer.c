#include <stdio.h>
#include <stdint.h>
#include "coroutine.h"

#define BUF_SIZE 10
#define N_PRODUCE 100
#define N_PRODUCER 400
#define N_CONSUMER 400

int buffer[BUF_SIZE];
int head = 0, tail = 0;
int count = 0;

semaphore_t sem_empty;
semaphore_t sem_full;
semaphore_t sem_mutex;

void put(int val) {
  buffer[tail] = val;
  tail = (tail + 1) % BUF_SIZE;
  count++;
}

int get() {
  int val = buffer[head];
  head = (head + 1) % BUF_SIZE;
  count--;
  return val;
}

void producer(void *arg) {
  int id = (int)(intptr_t)arg;
  for (int i = 0; i < N_PRODUCE; ++i) {
    sem_down(sem_empty);
    sem_down(sem_mutex);

    int val = id * 1000 + i;
    put(val);
#ifdef PRINT
    printf("[P%d] produce %d\n", id, val);
#endif
    sem_up(sem_mutex);
    sem_up(sem_full);
    co_yield();
  }
}

void consumer(void *arg) {
  int id = (int)(intptr_t)arg;
  for (int i = 0; i < N_PRODUCE * N_PRODUCER / N_CONSUMER; ++i) {
    sem_down(sem_full);
    sem_down(sem_mutex);

    int val = get();
#ifdef PRINT
    printf("        [C%d] consume %d\n", id, val);
#endif
    sem_up(sem_mutex);
    sem_up(sem_empty);
    co_yield();
  }
}

int main() {
  // 初始化信号量
  sem_empty = sem_create(BUF_SIZE);    // 初始空槽数量 = 缓冲区大小
  sem_full = sem_create(0);            // 初始满槽数量 = 0
  sem_mutex = sem_create(1);           // 互斥信号量初始为1

  coroutine_t producers[N_PRODUCER];
  coroutine_t consumers[N_CONSUMER];

  // 创建生产者协程
  for (int i = 0; i < N_PRODUCER; ++i) {
    char name[20];
    snprintf(name, sizeof(name), "producer-%d", i);
    producers[i] = co_start(name, producer, (void*)(intptr_t)i);
  }

  // 创建消费者协程
  for (int i = 0; i < N_CONSUMER; ++i) {
    char name[20];
    snprintf(name, sizeof(name), "consumer-%d", i);
    consumers[i] = co_start(name, consumer, (void*)(intptr_t)i);
  }

  // 等待所有生产者完成
  for (int i = 0; i < N_PRODUCER; ++i) {
    co_wait(producers[i]);
  }

  // 等待所有消费者完成
  for (int i = 0; i < N_CONSUMER; ++i) {
    co_wait(consumers[i]);
  }

  // 清理资源
  sem_destroy(sem_empty);
  sem_destroy(sem_full);
  sem_destroy(sem_mutex);

  printf("Finished. Final buffer count = %d\n", count);
  return 0;
}