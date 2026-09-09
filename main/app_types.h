#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* The LED frame points to magnetic north in this mode. */
    APP_MODE_NORTH = 0,
    /* The LED frame points from the current GPS fix to the configured target. */
    APP_MODE_TARGET = 1,
} app_mode_t;

typedef struct {
    /* HMC5883L values are already converted to milli-gauss by the driver. */
    float x_mg;
    float y_mg;
    float z_mg;
    int64_t timestamp_us;
} mag_sample_t;

typedef struct {
    /* The GPS task publishes the newest RMC-derived fix as a latest-value item. */
    bool rmc_seen;
    bool valid;
    float latitude_deg;
    float longitude_deg;
    int64_t last_sentence_us;
    int64_t last_fix_us;
    uint32_t rmc_count;
} gps_fix_t;

typedef struct {
    /* One immutable UI snapshot keeps the navigation and display tasks decoupled. */
    app_mode_t mode;
    uint8_t status;
    bool magnetometer_valid;
    bool gps_fix_valid;
    /* False when Target mode has no enabled, validated NVS target. */
    bool target_config_valid;
    bool calibration_valid;
    float heading_deg;
    float target_bearing_deg;
    float calibration_confidence;
    uint32_t calibration_age_s;
    uint16_t calibration_samples;
    uint8_t calibration_coverage_percent;
} ui_snapshot_t;
