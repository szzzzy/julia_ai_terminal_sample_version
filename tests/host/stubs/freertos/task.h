#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
unsigned ulTaskNotifyTake(int clear, TickType_t wait);
static inline void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
