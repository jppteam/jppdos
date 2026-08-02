/* Minimal FreeRTOS shim so jpp_http_server_core.c can run on the host. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

int g_log_quiet = 0;

typedef struct { void (*fn)(void *); void *arg; } trampoline_t;

static void *trampoline(void *p)
{
    trampoline_t *t = (trampoline_t *)p;
    t->fn(t->arg);
    free(t);
    return NULL;
}

TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name,
                               uint32_t stack_bytes, void *arg,
                               unsigned prio, StackType_t *stack,
                               StaticTask_t *tcb)
{
    (void)name; (void)stack_bytes; (void)prio; (void)stack;
    trampoline_t *t = malloc(sizeof(*t));
    t->fn = fn; t->arg = arg;
    if (pthread_create(&tcb->thread, NULL, trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    tcb->started = 1;
    return tcb;
}

/* The server task calls vTaskSuspend(NULL) once it has stopped serving; on the
   host that maps to the thread ending, and vTaskDelete to joining it.  The
   invariant under test is the same: after vTaskDelete returns, nothing is
   running on the pool memory any more. */
void vTaskSuspend(TaskHandle_t t) { (void)t; pthread_exit(NULL); }

void vTaskDelete(TaskHandle_t t)
{
    if (t != NULL && t->started) {
        pthread_join(t->thread, NULL);
        t->started = 0;
    }
}

void vTaskDelay(TickType_t ticks)
{
    if (ticks == portMAX_DELAY) { for (;;) { sleep(1); } }
    usleep((useconds_t)ticks * 1000u);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf)
{
    pthread_mutex_init(&buf->m, NULL);
    return buf;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)ticks;
    return pthread_mutex_lock(&s->m) == 0;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    return pthread_mutex_unlock(&s->m) == 0;
}

void vSemaphoreDelete(SemaphoreHandle_t s) { pthread_mutex_destroy(&s->m); }
