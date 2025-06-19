#ifndef COROUTINE_IN_C_COROUTINE_H
#define COROUTINE_IN_C_COROUTINE_H

typedef struct Goroutine *coroutine_t;
typedef struct Semaphore *semaphore_t;

coroutine_t co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(coroutine_t g);

semaphore_t sem_create(int cnt);
void sem_up(semaphore_t sem);
void sem_down(semaphore_t sem);
void sem_destroy(semaphore_t sem);

#endif //COROUTINE_IN_C_COROUTINE_H
