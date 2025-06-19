#ifndef COROUTINE_IN_C_COROUTINE_H
#define COROUTINE_IN_C_COROUTINE_H

typedef struct Goroutine *coroutine_t;

coroutine_t co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(coroutine_t g);

#endif //COROUTINE_IN_C_COROUTINE_H
