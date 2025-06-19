#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "coroutine.h"

#define LAYERS 10       // 层数
#define WIDTH  1000      // 每层宽度
#define WORK   100000    // 每个节点计算量

struct node_arg {
  int layer;
  int pos;
  int dep_num;
  coroutine_t *deps;
  double result;
};

void do_some_work(int idx, double *result) {
  double acc = idx;
  for (int i = 0; i < WORK; ++i)
    acc = acc * 1.0000001 + 0.5;
  *result = acc;
}

void dag_worker(void *arg) {
  struct node_arg *narg = (struct node_arg*)arg;
  for (int i = 0; i < narg->dep_num; ++i)
    co_wait(narg->deps[i]);
  do_some_work(narg->layer * WIDTH + narg->pos, &narg->result);
  free(narg->deps);
  free(narg);
}

coroutine_t nodes[LAYERS][WIDTH] = {{0}};

void omain(void *) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  // 创建所有节点
  for (int l = 0; l < LAYERS; ++l) {
    for (int w = 0; w < WIDTH; ++w) {
      struct node_arg *arg = malloc(sizeof(struct node_arg));
      arg->layer = l;
      arg->pos = w;

      // 0层没有依赖，其它层依赖上一层所有节点（可改为部分依赖）
      if (l == 0) {
        arg->dep_num = 0;
        arg->deps = NULL;
      } else {
        arg->dep_num = WIDTH;
        arg->deps = malloc(sizeof(coroutine_t) * WIDTH);
        for (int k = 0; k < WIDTH; ++k)
          arg->deps[k] = nodes[l-1][k];
      }
      nodes[l][w] = co_start("dag", dag_worker, arg);
    }
  }

  // 等待最后一层全部节点完成
  for (int w = 0; w < WIDTH; ++w) {
    co_wait(nodes[LAYERS-1][w]);
  }

  // 释放其它层的节点
  for (int l = 0; l < LAYERS-1; ++l)
    for (int w = 0; w < WIDTH; ++w)

  clock_gettime(CLOCK_MONOTONIC, &end);
  double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
  printf("DAG layers: %d, width: %d, total coroutines: %d\n", LAYERS, WIDTH, LAYERS*WIDTH);
  printf("Total time: %.6f s\n", sec);
}

int main() {
  coroutine_t g_main = co_start("main", omain, NULL);
  co_wait(g_main);
  return 0;
}