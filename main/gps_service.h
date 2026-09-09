#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t gps_service_init(void);
void gps_service_task(void *argument);
