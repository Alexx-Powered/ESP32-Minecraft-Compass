#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "app_types.h"

#define CALIBRATION_RECORD_MAGIC 0x324C4143UL /* "CAL2" little-endian */
#define CALIBRATION_RECORD_VERSION 1U

typedef struct {
    /* Versioned NVS payload: offset removes hard-iron bias and m00..m11
     * whiten the measured 2-D ellipse before heading calculation. */
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    float offset_x;
    float offset_y;
    float m00;
    float m01;
    float m10;
    float m11;
    float confidence;
    float axis_ratio;
    float radial_cv;
    uint32_t checksum;
} calibration_data_t;

typedef struct {
    /* Snapshot copied under the manager mutex; readers never hold the lock
     * while doing trigonometry or rendering. */
    calibration_data_t data;
    bool valid;
    int64_t applied_at_us;
    uint16_t collected_samples;
    uint8_t coverage_percent;
    float latest_fit_confidence;
} calibration_snapshot_t;

typedef struct {
    SemaphoreHandle_t lock;
    calibration_data_t current;
    bool valid;
    bool storage_available;
    int64_t applied_at_us;
    int64_t last_saved_at_us;
    uint16_t collected_samples;
    uint8_t coverage_percent;
    float latest_fit_confidence;
} calibration_manager_t;

typedef struct {
    calibration_manager_t *manager;
    QueueHandle_t sample_queue;
} calibration_task_config_t;

esp_err_t calibration_manager_init(calibration_manager_t *manager,
                                   bool storage_available);
void calibration_manager_get(calibration_manager_t *manager,
                             calibration_snapshot_t *snapshot);
void calibration_apply(const calibration_snapshot_t *snapshot,
                       float raw_x, float raw_y,
                       float *corrected_x, float *corrected_y);
void calibration_task(void *argument);
