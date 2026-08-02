#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define portMAX_DELAY      ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
#define tskIDLE_PRIORITY   0

typedef struct { pthread_t thread; int started; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;

typedef struct { pthread_mutex_t m; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
