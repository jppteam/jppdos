#pragma once
#include "freertos/FreeRTOS.h"

TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name,
                               uint32_t stack_bytes, void *arg,
                               unsigned prio, StackType_t *stack,
                               StaticTask_t *tcb);
void vTaskDelete(TaskHandle_t t);
void vTaskSuspend(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
