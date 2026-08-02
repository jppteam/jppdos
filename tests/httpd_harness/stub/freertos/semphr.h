#pragma once
#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
void              vSemaphoreDelete(SemaphoreHandle_t s);
