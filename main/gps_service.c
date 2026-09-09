#include "gps_service.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "app_types.h"

static const char *TAG = "gps";

static int hex_digit_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static bool checksum_is_valid(const char *sentence)
{
    /* NMEA uses an XOR of the payload between '$' and '*'; reject malformed
     * lines before touching the shared GPS state. */
    if (sentence == NULL || sentence[0] != '$') {
        return false;
    }

    const char *star = strchr(sentence, '*');
    if (star == NULL || star[1] == '\0' || star[2] == '\0') {
        return false;
    }
    const int high = hex_digit_value(star[1]);
    const int low = hex_digit_value(star[2]);
    if (high < 0 || low < 0) {
        return false;
    }

    uint8_t calculated = 0U;
    for (const char *cursor = sentence + 1; cursor < star; ++cursor) {
        calculated ^= (uint8_t)*cursor;
    }
    return calculated == (uint8_t)((high << 4) | low);
}

static bool parse_coordinate(const char *text, char hemisphere,
                             bool latitude, float *result)
{
    if (text == NULL || text[0] == '\0' || result == NULL) {
        return false;
    }

    char *end = NULL;
    const float raw = strtof(text, &end);
    if (end == text || *end != '\0' || !isfinite(raw) || raw < 0.0f) {
        return false;
    }

    const int degrees = (int)(raw / 100.0f);
    const float minutes = raw - (float)degrees * 100.0f;
    const int maximum_degrees = latitude ? 90 : 180;
    if (minutes < 0.0f || minutes >= 60.0f ||
        degrees > maximum_degrees ||
        (degrees == maximum_degrees && minutes > 0.0f)) {
        return false;
    }

    const bool positive = latitude ? hemisphere == 'N' : hemisphere == 'E';
    const bool negative = latitude ? hemisphere == 'S' : hemisphere == 'W';
    if (!positive && !negative) {
        return false;
    }

    const float decimal = (float)degrees + minutes / 60.0f;
    *result = negative ? -decimal : decimal;
    return true;
}

static bool parse_rmc(char *sentence, gps_fix_t *state)
{
    /* RMC supplies both validity and position.  The parser accepts common
     * GPS talker IDs while preserving the last known fix on bad sentences. */
    if (sentence == NULL || state == NULL || !checksum_is_valid(sentence)) {
        return false;
    }

    char *star = strchr(sentence, '*');
    *star = '\0';

    char *fields[12] = {0};
    size_t field_count = 0U;
    char *cursor = sentence;
    while (cursor != NULL && field_count < sizeof(fields) / sizeof(fields[0])) {
        fields[field_count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }

    if (field_count < 10U ||
        (strcmp(fields[0], "$GPRMC") != 0 &&
         strcmp(fields[0], "$GNRMC") != 0)) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    state->rmc_seen = true;
    state->last_sentence_us = now_us;
    state->rmc_count++;
    if (fields[2][0] != 'A') {
        state->valid = false;
        return true;
    }

    float latitude = 0.0f;
    float longitude = 0.0f;
    if (!parse_coordinate(fields[3], fields[4][0], true, &latitude) ||
        !parse_coordinate(fields[5], fields[6][0], false, &longitude)) {
        state->valid = false;
        return false;
    }

    state->latitude_deg = latitude;
    state->longitude_deg = longitude;
    state->last_fix_us = now_us;
    state->valid = true;
    return true;
}

esp_err_t gps_service_init(void)
{
    /* UART is configured once here; the task below only reads complete lines
     * and publishes a latest-value snapshot, so navigation never waits for GPS. */
    const uart_config_t uart_config = {
        .baud_rate = APP_GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(APP_GPS_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(APP_GPS_UART_PORT,
                       APP_GPS_TX_GPIO, APP_GPS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_driver_install(APP_GPS_UART_PORT, 2048, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART%d ready: TX=GPIO%d RX=GPIO%d %d baud",
                 (int)APP_GPS_UART_PORT, (int)APP_GPS_TX_GPIO,
                 (int)APP_GPS_RX_GPIO, APP_GPS_BAUD_RATE);
    }
    return err;
}

void gps_service_task(void *argument)
{
    /* A short UART timeout keeps this priority-5 task responsive without busy
     * spinning when no NMEA data is arriving. */
    QueueHandle_t output_queue = (QueueHandle_t)argument;
    if (output_queue == NULL) {
        ESP_LOGE(TAG, "GPS output queue is null");
        vTaskDelete(NULL);
        return;
    }

    gps_fix_t state = {0};
    xQueueOverwrite(output_queue, &state);

    char line[APP_GPS_LINE_MAX_LENGTH];
    size_t line_length = 0U;
    uint8_t rx_data[256];

    while (true) {
        const int received = uart_read_bytes(APP_GPS_UART_PORT,
                                             rx_data, sizeof(rx_data),
                                             pdMS_TO_TICKS(20));
        for (int i = 0; i < received; ++i) {
            const char character = (char)rx_data[i];
            if (character == '$') {
                line_length = 0U;
                line[line_length++] = character;
                continue;
            }
            if (character == '\r') {
                continue;
            }
            if (character == '\n') {
                if (line_length > 0U) {
                    line[line_length] = '\0';
                    if (parse_rmc(line, &state)) {
                        xQueueOverwrite(output_queue, &state);
                        ESP_LOGD(TAG, "RMC #%lu %s lat=%.6f lon=%.6f",
                                 (unsigned long)state.rmc_count,
                                 state.valid ? "FIX" : "NOFIX",
                                 state.latitude_deg, state.longitude_deg);
                    }
                }
                line_length = 0U;
                continue;
            }

            if (line_length < sizeof(line) - 1U) {
                line[line_length++] = character;
            } else {
                line_length = 0U;
            }
        }
    }
}
