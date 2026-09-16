#pragma once
#include <stddef.h>
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *value, unsigned wait);
int xQueueReceive(QueueHandle_t queue, void *value, unsigned wait);
void vQueueDelete(QueueHandle_t queue);
