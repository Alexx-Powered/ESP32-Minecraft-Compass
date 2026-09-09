#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hmc5883l.h"
#include "i2cdev.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_types.h"
#include "calibration.h"
#include "compass_angle.h"
#include "config_portal.h"
#include "geo_config.h"
#include "gps_service.h"
#include "oled_display.h"
#include "ui_service.h"

static const char *TAG = "minecraft_compass";

typedef struct {
    /* Queues carry latest navigation/UI state; the calibration queue is bounded
     * so a slow fit never applies back-pressure to the 50 Hz sensor loop. */
    QueueHandle_t magnetometer_navigation_queue;
    QueueHandle_t magnetometer_calibration_queue;
    QueueHandle_t gps_queue;
    QueueHandle_t mode_queue;
    QueueHandle_t ui_queue;
    SemaphoreHandle_t i2c_mutex;

    hmc5883l_dev_t magnetometer;
    bool magnetometer_descriptor_ready;
    bool magnetometer_configured;

    calibration_manager_t calibration;
    geo_config_manager_t geo_config;
    calibration_task_config_t calibration_task_config;
    config_portal_config_t portal_config;
    ui_task_config_t ui_task_config;
} app_context_t;

static app_context_t s_app;

static float normalize_degrees(float degrees)
{
    degrees = fmodf(degrees, 360.0f);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

static float target_bearing_degrees(float latitude_1, float longitude_1,
                                    float latitude_2, float longitude_2)
{
    /* Initial great-circle bearing, measured clockwise from true north. */
    const float degrees_to_radians = (float)M_PI / 180.0f;
    const float latitude_1_rad = latitude_1 * degrees_to_radians;
    const float latitude_2_rad = latitude_2 * degrees_to_radians;
    const float longitude_delta =
        (longitude_2 - longitude_1) * degrees_to_radians;
    const float y = sinf(longitude_delta) * cosf(latitude_2_rad);
    const float x = cosf(latitude_1_rad) * sinf(latitude_2_rad) -
                    sinf(latitude_1_rad) * cosf(latitude_2_rad) *
                        cosf(longitude_delta);
    return normalize_degrees(atan2f(y, x) * 180.0f / (float)M_PI);
}

static uint8_t quantize_with_hysteresis(float relative_angle,
                                        uint8_t current_status)
{
    /* Use the fitted Voronoi intervals and apply hysteresis beyond the actual
     * interval boundary, rather than around a fictitious 11.25-degree grid. */
    return compass_status_with_hysteresis(
        relative_angle, current_status, APP_COMPASS_ANGLE_HYSTERESIS_DEG);
}

static esp_err_t configure_magnetometer(app_context_t *app)
{
    /* HMC5883L and SSD1306 share the same bus; the caller holds i2c_mutex.  The
     * local component also makes initialization deterministic on ESP-IDF 5.x. */
    if (!app->magnetometer_descriptor_ready) {
        memset(&app->magnetometer, 0, sizeof(app->magnetometer));
        app->magnetometer.i2c_dev.cfg.sda_pullup_en = true;
        app->magnetometer.i2c_dev.cfg.scl_pullup_en = true;
        esp_err_t err = hmc5883l_init_desc(&app->magnetometer,
                                           APP_I2C_PORT,
                                           APP_I2C_SDA_GPIO,
                                           APP_I2C_SCL_GPIO);
        if (err != ESP_OK) {
            return err;
        }
        app->magnetometer_descriptor_ready = true;
    }

    esp_err_t err = hmc5883l_init(&app->magnetometer);
    if (err == ESP_OK) {
        err = hmc5883l_set_samples_averaged(&app->magnetometer,
                                            HMC5883L_SAMPLES_8);
    }
    if (err == ESP_OK) {
        err = hmc5883l_set_data_rate(&app->magnetometer,
                                     HMC5883L_DATA_RATE_75_00);
    }
    if (err == ESP_OK) {
        err = hmc5883l_set_gain(&app->magnetometer,
                                HMC5883L_GAIN_1090);
    }
    if (err == ESP_OK) {
        err = hmc5883l_set_opmode(&app->magnetometer,
                                  HMC5883L_MODE_CONTINUOUS);
    }
    app->magnetometer_configured = err == ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HMC5883L ready: I2C%d SDA=GPIO%d SCL=GPIO%d",
                 (int)APP_I2C_PORT, (int)APP_I2C_SDA_GPIO,
                 (int)APP_I2C_SCL_GPIO);
    }
    return err;
}

static bool magnetometer_sample_is_valid(const hmc5883l_data_t *data)
{
    return isfinite(data->x) && isfinite(data->y) && isfinite(data->z) &&
           fabsf(data->x) <= APP_MAG_MAX_ABS_MG &&
           fabsf(data->y) <= APP_MAG_MAX_ABS_MG &&
           fabsf(data->z) <= APP_MAG_MAX_ABS_MG;
}

static void magnetometer_task(void *argument)
{
    /* Highest application priority: bounded I2C access, then publish a sample
     * to navigation and calibration without waiting for either consumer. */
    app_context_t *app = (app_context_t *)argument;
    TickType_t next_wake = xTaskGetTickCount();
    int64_t last_retry_us = 0;
    unsigned consecutive_failures = 0U;

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        if (!app->magnetometer_configured &&
            now_us - last_retry_us >= 2000000LL) {
            last_retry_us = now_us;
            if (xSemaphoreTake(app->i2c_mutex,
                               pdMS_TO_TICKS(250)) == pdTRUE) {
                const esp_err_t err = configure_magnetometer(app);
                xSemaphoreGive(app->i2c_mutex);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "HMC5883L retry failed: %s",
                             esp_err_to_name(err));
                } else {
                    consecutive_failures = 0U;
                }
            }
        }

        if (app->magnetometer_configured &&
            xSemaphoreTake(app->i2c_mutex,
                           pdMS_TO_TICKS(100)) == pdTRUE) {
            hmc5883l_data_t data;
            const esp_err_t err = hmc5883l_get_data(&app->magnetometer,
                                                     &data);
            xSemaphoreGive(app->i2c_mutex);
            if (err == ESP_OK && magnetometer_sample_is_valid(&data)) {
                const mag_sample_t sample = {
                    .x_mg = data.x,
                    .y_mg = data.y,
                    .z_mg = data.z,
                    .timestamp_us = esp_timer_get_time(),
                };
                xQueueOverwrite(app->magnetometer_navigation_queue, &sample);
                (void)xQueueSend(app->magnetometer_calibration_queue,
                                 &sample, 0);
                consecutive_failures = 0U;
            } else {
                consecutive_failures++;
                if (consecutive_failures == 1U ||
                    (consecutive_failures % 50U) == 0U) {
                    ESP_LOGW(TAG, "HMC5883L sample rejected: %s",
                             err == ESP_OK ? "implausible value"
                                           : esp_err_to_name(err));
                }
                if (consecutive_failures >= 5U) {
                    app->magnetometer_configured = false;
                }
            }
        }

        vTaskDelayUntil(&next_wake,
                        pdMS_TO_TICKS(APP_MAG_SAMPLE_PERIOD_MS));
    }
}

static void button_task(void *argument)
{
    /* Polling keeps the button independent of an ISR and makes debounce timing
     * explicit; a low level selects the next mode exactly once per press. */
    app_context_t *app = (app_context_t *)argument;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << APP_MODE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    int last_raw = gpio_get_level(APP_MODE_BUTTON_GPIO);
    int stable = last_raw;
    TickType_t changed_at = xTaskGetTickCount();
    TickType_t next_wake = changed_at;

    while (true) {
        const int raw = gpio_get_level(APP_MODE_BUTTON_GPIO);
        const TickType_t now = xTaskGetTickCount();
        if (raw != last_raw) {
            last_raw = raw;
            changed_at = now;
        }
        if (raw != stable &&
            now - changed_at >= pdMS_TO_TICKS(APP_MODE_BUTTON_DEBOUNCE_MS)) {
            stable = raw;
            if (stable == 0) {
                app_mode_t mode = APP_DEFAULT_MODE;
                (void)xQueuePeek(app->mode_queue, &mode, 0);
                mode = mode == APP_MODE_NORTH
                           ? APP_MODE_TARGET
                           : APP_MODE_NORTH;
                xQueueOverwrite(app->mode_queue, &mode);
                ESP_LOGI(TAG, "Mode changed to %s",
                         mode == APP_MODE_NORTH ? "NORTH" : "TARGET");
            }
        }
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(10));
    }
}

static void navigation_task(void *argument)
{
    /* This task owns heading filtering and status selection.  It consumes the
     * newest sensor/GPS values and overwrites one UI snapshot for the renderer. */
    app_context_t *app = (app_context_t *)argument;
    mag_sample_t latest_magnetometer = {0};
    gps_fix_t latest_gps = {0};
    app_mode_t mode = APP_DEFAULT_MODE;
    bool have_magnetometer = false;
    bool filter_initialized = false;
    float filtered_cosine = 1.0f;
    float filtered_sine = 0.0f;
    float heading_degrees = 0.0f;
    uint8_t current_status = 0U;
    geo_fence_state_t previous_fence_states[GEO_FENCE_COUNT] = {0};
    bool fence_state_initialized[GEO_FENCE_COUNT] = {false};

    while (true) {
        mag_sample_t sample;
        const bool new_sample =
            xQueueReceive(app->magnetometer_navigation_queue, &sample,
                          pdMS_TO_TICKS(50)) == pdTRUE;
        if (new_sample) {
            latest_magnetometer = sample;
            have_magnetometer = true;

            calibration_snapshot_t calibration;
            calibration_manager_get(&app->calibration, &calibration);
            float corrected_x = 0.0f;
            float corrected_y = 0.0f;
            calibration_apply(&calibration,
                              sample.x_mg, sample.y_mg,
                              &corrected_x, &corrected_y);

            /* The axis convention matches the original compass frame mapping.
             * Apply the user-adjustable sensor mounting offset before magnetic
             * declination so APP_SENSOR_ANGLE_OFFSET_DEG is the first knob. */
            float heading = atan2f(corrected_x, -corrected_y) *
                            180.0f / (float)M_PI;
            heading = normalize_degrees(
                heading + APP_SENSOR_ANGLE_OFFSET_DEG +
                APP_MAGNETIC_DECLINATION_DEG);
            const float heading_radians = heading * (float)M_PI / 180.0f;
            const float sample_cosine = cosf(heading_radians);
            const float sample_sine = sinf(heading_radians);
            if (!filter_initialized) {
                filtered_cosine = sample_cosine;
                filtered_sine = sample_sine;
                filter_initialized = true;
            } else {
                filtered_cosine += APP_HEADING_FILTER_ALPHA *
                                   (sample_cosine - filtered_cosine);
                filtered_sine += APP_HEADING_FILTER_ALPHA *
                                 (sample_sine - filtered_sine);
                const float norm = hypotf(filtered_cosine, filtered_sine);
                if (norm > 1e-6f) {
                    filtered_cosine /= norm;
                    filtered_sine /= norm;
                }
            }
            heading_degrees = normalize_degrees(
                atan2f(filtered_sine, filtered_cosine) *
                180.0f / (float)M_PI);
        }

        (void)xQueuePeek(app->gps_queue, &latest_gps, 0);
        (void)xQueuePeek(app->mode_queue, &mode, 0);
        const int64_t now_us = esp_timer_get_time();
        const bool magnetometer_valid = have_magnetometer &&
            now_us - latest_magnetometer.timestamp_us <=
                APP_MAG_STALE_AFTER_US;
        const bool gps_valid = latest_gps.valid &&
            now_us - latest_gps.last_fix_us <= APP_GPS_FIX_MAX_AGE_US;

        /* The HTTP server swaps a complete validated snapshot under its own
         * short mutex, so navigation never observes a partly updated target or
         * fence. GPS fixes are WGS84; the geo module performs the one-way
         * WGS84-to-GCJ02 conversion only when a configured object requires it. */
        geo_config_snapshot_t geo_snapshot;
        geo_config_manager_get(&app->geo_config, &geo_snapshot);
        const bool target_config_valid =
            geo_snapshot.target.configured && geo_snapshot.target.enabled;

        double origin_latitude = 0.0;
        double origin_longitude = 0.0;
        double target_latitude = 0.0;
        double target_longitude = 0.0;
        const bool target_resolved = gps_valid && target_config_valid &&
            geo_config_resolve_target(
                &geo_snapshot,
                latest_gps.latitude_deg, latest_gps.longitude_deg,
                &origin_latitude, &origin_longitude,
                &target_latitude, &target_longitude);

        geo_fence_state_t fence_states[GEO_FENCE_COUNT];
        geo_config_evaluate_fences(
            &geo_snapshot, gps_valid,
            latest_gps.latitude_deg, latest_gps.longitude_deg,
            fence_states);
        for (size_t index = 0U; index < GEO_FENCE_COUNT; ++index) {
            if (!fence_state_initialized[index] ||
                fence_states[index] != previous_fence_states[index]) {
                ESP_LOGI(TAG, "Fence%u state: %s",
                         (unsigned)(index + 1U),
                         geo_fence_state_name(fence_states[index]));
                previous_fence_states[index] = fence_states[index];
                fence_state_initialized[index] = true;
            }
        }

        float bearing = 0.0f;
        if (!magnetometer_valid) {
            current_status = 0U;
        } else if (mode == APP_MODE_NORTH) {
            current_status = quantize_with_hysteresis(
                normalize_degrees(-heading_degrees), current_status);
        } else if (target_resolved) {
            bearing = target_bearing_degrees(
                (float)origin_latitude, (float)origin_longitude,
                (float)target_latitude, (float)target_longitude);
            current_status = quantize_with_hysteresis(
                normalize_degrees(bearing - heading_degrees),
                current_status);
        } else {
            current_status = 0U;
        }

        calibration_snapshot_t calibration;
        calibration_manager_get(&app->calibration, &calibration);
        const uint32_t age_seconds = calibration.valid &&
                                     now_us >= calibration.applied_at_us
            ? (uint32_t)((now_us - calibration.applied_at_us) / 1000000LL)
            : 0U;
        const ui_snapshot_t ui = {
            .mode = mode,
            .status = current_status,
            .magnetometer_valid = magnetometer_valid,
            .gps_fix_valid = gps_valid,
            .target_config_valid = target_config_valid,
            .calibration_valid = calibration.valid,
            .heading_deg = heading_degrees,
            .target_bearing_deg = bearing,
            .calibration_confidence = calibration.valid
                ? calibration.data.confidence
                : calibration.latest_fit_confidence,
            .calibration_age_s = age_seconds,
            .calibration_samples = calibration.collected_samples,
            .calibration_coverage_percent = calibration.coverage_percent,
        };
        xQueueOverwrite(app->ui_queue, &ui);
    }
}

static void require_task(BaseType_t result, const char *name)
{
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Could not create task %s", name);
        abort();
    }
}

void app_main(void)
{
    /* Startup tolerates missing peripherals and never erases NVS as recovery.
     * The only intentional wait is the short WiFi-credential splash before the
     * worker tasks start; afterwards each service retries/refreshes itself. */
    memset(&s_app, 0, sizeof(s_app));

    esp_err_t nvs_err = nvs_flash_init();
    const bool nvs_available = nvs_err == ESP_OK;
    if (!nvs_available) {
        ESP_LOGE(TAG,
                 "NVS initialization failed: %s; stored calibration is preserved but unavailable",
                 esp_err_to_name(nvs_err));
    }

    /* Length-one queues implement latest-value semantics; calibration has a
     * small FIFO so short bursts are not lost while its fit runs. */
    s_app.magnetometer_navigation_queue = xQueueCreate(1, sizeof(mag_sample_t));
    s_app.magnetometer_calibration_queue = xQueueCreate(64, sizeof(mag_sample_t));
    s_app.gps_queue = xQueueCreate(1, sizeof(gps_fix_t));
    s_app.mode_queue = xQueueCreate(1, sizeof(app_mode_t));
    s_app.ui_queue = xQueueCreate(1, sizeof(ui_snapshot_t));
    s_app.i2c_mutex = xSemaphoreCreateMutex();
    if (s_app.magnetometer_navigation_queue == NULL ||
        s_app.magnetometer_calibration_queue == NULL ||
        s_app.gps_queue == NULL || s_app.mode_queue == NULL ||
        s_app.ui_queue == NULL || s_app.i2c_mutex == NULL) {
        ESP_LOGE(TAG, "Could not allocate queues/mutexes");
        abort();
    }

    app_mode_t initial_mode = APP_DEFAULT_MODE;
    gps_fix_t initial_gps = {0};
    xQueueOverwrite(s_app.mode_queue, &initial_mode);
    xQueueOverwrite(s_app.gps_queue, &initial_gps);
    ESP_ERROR_CHECK(calibration_manager_init(&s_app.calibration,
                                             nvs_available));
    ESP_ERROR_CHECK(geo_config_manager_init(&s_app.geo_config,
                                            nvs_available));

    ESP_ERROR_CHECK(i2cdev_init());
    esp_err_t hmc_err = configure_magnetometer(&s_app);
    if (hmc_err != ESP_OK) {
        ESP_LOGW(TAG, "HMC5883L initial probe failed: %s; background retry enabled",
                 esp_err_to_name(hmc_err));
    }

    oled_display_t *display = oled_display_create();
    if (display == NULL) {
        ESP_LOGW(TAG, "OLED unavailable; WS2812 and logging remain active");
    }

    const esp_err_t gps_err = gps_service_init();
    if (gps_err != ESP_OK) {
        ESP_LOGE(TAG, "GPS UART initialization failed: %s",
                 esp_err_to_name(gps_err));
    }

    /* The local configuration page is deliberately self-contained: clients
     * connect directly to the WPA2 SoftAP and no cloud/CDN is required. The
     * random password is persisted separately from compass calibration data. */
    s_app.portal_config.geo_manager = &s_app.geo_config;
    s_app.portal_config.gps_queue = s_app.gps_queue;
    char ap_ssid[33] = {0};
    char ap_password[65] = {0};
    const esp_err_t portal_err = config_portal_start(
        &s_app.portal_config,
        ap_ssid, sizeof(ap_ssid),
        ap_password, sizeof(ap_password));
    if (portal_err != ESP_OK) {
        ESP_LOGE(TAG, "Configuration portal unavailable: %s; compass continues",
                 esp_err_to_name(portal_err));
    } else if (display != NULL &&
               xSemaphoreTake(s_app.i2c_mutex,
                              pdMS_TO_TICKS(250)) == pdTRUE) {
        const esp_err_t display_err = oled_display_show_wifi_setup(
            display, ap_ssid, ap_password);
        xSemaphoreGive(s_app.i2c_mutex);
        if (display_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not show WiFi credentials: %s",
                     esp_err_to_name(display_err));
        } else {
            vTaskDelay(pdMS_TO_TICKS(APP_CONFIG_CREDENTIALS_DISPLAY_MS));
        }
    }

    s_app.calibration_task_config.manager = &s_app.calibration;
    s_app.calibration_task_config.sample_queue =
        s_app.magnetometer_calibration_queue;
    s_app.ui_task_config.snapshot_queue = s_app.ui_queue;
    s_app.ui_task_config.i2c_mutex = s_app.i2c_mutex;
    s_app.ui_task_config.display = display;

    /* Priorities are ordered by freshness: sensor, navigation/GPS, button,
     * display, then the intentionally low-priority calibration fit. */
    require_task(xTaskCreate(magnetometer_task, "mag_acquire", 4096,
                             &s_app, 6, NULL), "mag_acquire");
    require_task(xTaskCreate(navigation_task, "navigation", 4096,
                             &s_app, 5, NULL), "navigation");
    if (gps_err == ESP_OK) {
        require_task(xTaskCreate(gps_service_task, "gps", 4096,
                                 s_app.gps_queue, 5, NULL), "gps");
    }
    require_task(xTaskCreate(button_task, "button", 2048,
                             &s_app, 4, NULL), "button");
    require_task(xTaskCreate(ui_service_task, "ui", 4096,
                             &s_app.ui_task_config, 3, NULL), "ui");
    require_task(xTaskCreate(calibration_task, "calibration", 8192,
                             &s_app.calibration_task_config, 2, NULL),
                 "calibration");

    ESP_LOGI(TAG,
             "Application started: button=GPIO%d WS2812=GPIO%d sensor_offset=%.1f deg config=%s",
             (int)APP_MODE_BUTTON_GPIO, (int)APP_WS2812_GPIO,
             APP_SENSOR_ANGLE_OFFSET_DEG,
             portal_err == ESP_OK ? "http://192.168.4.1" : "unavailable");
}
