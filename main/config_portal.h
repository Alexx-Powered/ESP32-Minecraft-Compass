#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "geo_config.h"

typedef struct {
    geo_config_manager_t *geo_manager;
    QueueHandle_t gps_queue;
} config_portal_config_t;

esp_err_t config_portal_start(const config_portal_config_t *config,
                              char *ssid,
                              size_t ssid_size,
                              char *password,
                              size_t password_size);
