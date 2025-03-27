#ifndef COROUTINE_IN_C_COROUTINE_H
#define COROUTINE_IN_C_COROUTINE_H

typedef struct co coroutine_t;

coroutine_t *co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(coroutine_t *co);
void co_resume(coroutine_t *co);
void co_free(coroutine_t *co);

#endif //COROUTINE_IN_C_COROUTINE_H
