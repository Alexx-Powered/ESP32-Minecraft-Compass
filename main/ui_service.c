#include "ui_service.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "app_config.h"
#include "app_types.h"
#include "compass_angle.h"
#include "compass_frames.h"

static const char *TAG = "ui";

#define INVALID_SPIN_BASE_STATES_PER_SECOND 18.0f
#define INVALID_SPIN_SMOOTHING_SECONDS 0.42f
#define INVALID_SPIN_MIN_CHANGE_US 550000ULL
#define INVALID_SPIN_MAX_CHANGE_US 1500000ULL
#define INVALID_SPIN_MAX_DELTA_SECONDS 0.25f

typedef struct {
    bool active;
    float angle_degrees;
    float velocity_degrees_per_second;
    float target_velocity_degrees_per_second;
    uint64_t previous_us;
    uint64_t next_target_change_us;
} invalid_spin_t;

static uint32_t random_below(uint32_t upper_exclusive)
{
    return upper_exclusive <= 1U ? 0U : esp_random() % upper_exclusive;
}

static uint32_t random_inclusive(uint32_t minimum, uint32_t maximum)
{
    return minimum + random_below(maximum - minimum + 1U);
}

static uint8_t frame_for_angle(float angle_degrees)
{
    /* Keep the status-0 animation on the same fitted intervals as navigation. */
    return compass_status_for_angle(angle_degrees);
}

static void choose_spin_target(invalid_spin_t *spin, uint64_t now_us)
{
    const float base_speed = INVALID_SPIN_BASE_STATES_PER_SECOND *
                             COMPASS_FITTED_AVERAGE_STEP_DEG;
    const float speed_scale =
        (float)random_inclusive(650U, 1350U) / 1000.0f;
    float direction = spin->target_velocity_degrees_per_second < 0.0f
                          ? -1.0f
                          : 1.0f;
    if (random_below(100U) < 42U) {
        direction = -direction;
    }
    spin->target_velocity_degrees_per_second =
        direction * base_speed * speed_scale;
    spin->next_target_change_us = now_us + random_inclusive(
        (uint32_t)INVALID_SPIN_MIN_CHANGE_US,
        (uint32_t)INVALID_SPIN_MAX_CHANGE_US);
}

static uint8_t update_invalid_spin(invalid_spin_t *spin)
{
    /* Status 0 means the compass cannot be trusted.  Keep the matrix visually
     * alive with a smooth, bounded random walk instead of a frozen arrow. */
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (!spin->active) {
        spin->angle_degrees = (float)random_inclusive(0U, 35999U) / 100.0f;
        const float direction = random_below(2U) == 0U ? -1.0f : 1.0f;
        spin->velocity_degrees_per_second =
            direction * INVALID_SPIN_BASE_STATES_PER_SECOND *
            COMPASS_FITTED_AVERAGE_STEP_DEG;
        spin->target_velocity_degrees_per_second =
            spin->velocity_degrees_per_second;
        spin->previous_us = now_us;
        spin->active = true;
        choose_spin_target(spin, now_us);
        return frame_for_angle(spin->angle_degrees);
    }

    uint64_t elapsed_us = now_us >= spin->previous_us
                              ? now_us - spin->previous_us
                              : 0U;
    float delta_seconds = (float)elapsed_us / 1000000.0f;
    if (delta_seconds > INVALID_SPIN_MAX_DELTA_SECONDS) {
        delta_seconds = INVALID_SPIN_MAX_DELTA_SECONDS;
    }
    spin->previous_us = now_us;
    if (now_us >= spin->next_target_change_us) {
        choose_spin_target(spin, now_us);
    }

    const float smoothing = 1.0f -
        expf(-delta_seconds / INVALID_SPIN_SMOOTHING_SECONDS);
    spin->velocity_degrees_per_second +=
        (spin->target_velocity_degrees_per_second -
         spin->velocity_degrees_per_second) * smoothing;
    spin->angle_degrees += spin->velocity_degrees_per_second * delta_seconds;
    spin->angle_degrees = fmodf(spin->angle_degrees, 360.0f);
    if (spin->angle_degrees < 0.0f) {
        spin->angle_degrees += 360.0f;
    }
    return frame_for_angle(spin->angle_degrees);
}

static uint8_t apply_brightness(uint8_t component)
{
    return (uint8_t)(((uint16_t)component *
                      APP_WS2812_BRIGHTNESS_PERCENT + 50U) / 100U);
}

static led_strip_handle_t create_led_strip(void)
{
    /* The frame table already contains the reverse physical order required by
     * the installed matrix, so the driver only has to emit linear LED indices. */
    const led_strip_config_t strip_config = {
        .strip_gpio_num = APP_WS2812_GPIO,
        .max_leds = COMPASS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = APP_WS2812_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                              &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 initialization failed: %s",
                 esp_err_to_name(err));
        return NULL;
    }
    (void)led_strip_clear(strip);
    ESP_LOGI(TAG, "WS2812 ready: GPIO%d, %u LEDs, brightness=%u%%",
             (int)APP_WS2812_GPIO, (unsigned)COMPASS_LED_COUNT,
             (unsigned)APP_WS2812_BRIGHTNESS_PERCENT);
    return strip;
}

static esp_err_t show_frame(led_strip_handle_t strip, uint8_t status)
{
    /* Apply a global brightness limit before writing the precomputed RGB frame. */
    if (strip == NULL || status == 0U || status > COMPASS_FRAME_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t *frame = COMPASS_LED_FRAMES[status - 1U];
    for (uint32_t led = 0U; led < COMPASS_LED_COUNT; ++led) {
        const uint32_t color = frame[led];
        const uint8_t red = apply_brightness((uint8_t)(color >> 16));
        const uint8_t green = apply_brightness((uint8_t)(color >> 8));
        const uint8_t blue = apply_brightness((uint8_t)color);
        esp_err_t err = led_strip_set_pixel(strip, led, red, green, blue);
        if (err != ESP_OK) {
            return err;
        }
    }
    return led_strip_refresh(strip);
}

static esp_err_t render_oled(oled_display_t *display,
                             const ui_snapshot_t *snapshot)
{
    /* OLED is a status/debug surface: mode, frame, sensor/GPS health and the
     * calibration age/progress are rendered without affecting LED timing. */
    char line[24];
    esp_err_t err = oled_display_clear(display);
    if (err != ESP_OK) {
        return err;
    }

    (void)snprintf(line, sizeof(line), "MODE: %s",
                   snapshot->mode == APP_MODE_NORTH ? "NORTH" : "TARGET");
    err = oled_display_draw_text(display, 2, 1, line, 1, true);
    if (err != ESP_OK) {
        return err;
    }

    (void)snprintf(line, sizeof(line), "STATUS:%02u",
                   (unsigned)snapshot->status);
    err = oled_display_draw_text(display, 2, 14, line, 2, true);
    if (err != ESP_OK) {
        return err;
    }

    if (snapshot->mode == APP_MODE_TARGET &&
        !snapshot->target_config_valid) {
        (void)snprintf(line, sizeof(line), "TARGET:NOT SET");
    } else if (!snapshot->magnetometer_valid) {
        (void)snprintf(line, sizeof(line), "HMC: WAIT/ERROR");
    } else if (snapshot->mode == APP_MODE_TARGET) {
        if (snapshot->gps_fix_valid) {
            (void)snprintf(line, sizeof(line), "GPS:FIX B:%5.1f",
                           snapshot->target_bearing_deg);
        } else {
            (void)snprintf(line, sizeof(line), "GPS:WAIT TARGET");
        }
    } else {
        (void)snprintf(line, sizeof(line), "HDG:%5.1f MAG",
                       snapshot->heading_deg);
    }
    err = oled_display_draw_text(display, 2, 37, line, 1, true);
    if (err != ESP_OK) {
        return err;
    }

    if (snapshot->calibration_valid) {
        const unsigned long age = snapshot->calibration_age_s > 999999U
                                      ? 999999UL
                                      : (unsigned long)snapshot->calibration_age_s;
        (void)snprintf(line, sizeof(line), "calibrated(%lus)", age);
    } else {
        (void)snprintf(line, sizeof(line), "calibrating(%u%%)",
                       (unsigned)snapshot->calibration_coverage_percent);
    }
    err = oled_display_draw_text(display, 2, 54, line, 1, true);
    if (err != ESP_OK) {
        return err;
    }
    return oled_display_flush(display);
}

void ui_service_task(void *argument)
{
    /* Lowest interactive UI priority.  WS2812 updates are non-blocking from
     * navigation's perspective; OLED transactions take the shared I2C mutex
     * only for a bounded 100 ms and are rate-limited to 5 Hz. */
    ui_task_config_t *config = (ui_task_config_t *)argument;
    if (config == NULL || config->snapshot_queue == NULL ||
        config->i2c_mutex == NULL) {
        ESP_LOGE(TAG, "Invalid UI task configuration");
        vTaskDelete(NULL);
        return;
    }

    led_strip_handle_t strip = create_led_strip();
    oled_display_t *display = config->display;
    ui_snapshot_t snapshot = {
        .mode = APP_DEFAULT_MODE,
    };
    invalid_spin_t spin = {0};
    uint8_t last_frame = 0U;
    int64_t last_oled_update_us = 0;
    unsigned oled_failures = 0U;

    while (true) {
        ui_snapshot_t latest;
        if (xQueueReceive(config->snapshot_queue, &latest,
                          pdMS_TO_TICKS(20)) == pdTRUE) {
            snapshot = latest;
        }

        uint8_t frame = snapshot.status;
        if (frame == 0U || frame > COMPASS_FRAME_COUNT) {
            frame = update_invalid_spin(&spin);
        } else {
            spin.active = false;
        }
        if (strip != NULL && frame != last_frame) {
            const esp_err_t err = show_frame(strip, frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "WS2812 update failed: %s",
                         esp_err_to_name(err));
            } else {
                last_frame = frame;
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (display != NULL &&
            now_us - last_oled_update_us >= 200000LL) {
            if (xSemaphoreTake(config->i2c_mutex,
                               pdMS_TO_TICKS(100)) == pdTRUE) {
                const esp_err_t err = render_oled(display, &snapshot);
                xSemaphoreGive(config->i2c_mutex);
                if (err != ESP_OK) {
                    oled_failures++;
                    ESP_LOGW(TAG, "OLED update failed (%u): %s",
                             oled_failures, esp_err_to_name(err));
                } else {
                    oled_failures = 0U;
                }
            }
            last_oled_update_us = now_us;
        }
    }
}
