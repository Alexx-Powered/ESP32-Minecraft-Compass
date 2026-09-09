#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "oled_display.h"

typedef struct {
    QueueHandle_t snapshot_queue;
    SemaphoreHandle_t i2c_mutex;
    oled_display_t *display;
} ui_task_config_t;

void ui_service_task(void *argument);
