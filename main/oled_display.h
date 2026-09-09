/* SSD1306 adapter using the I2C bus already created by i2cdev/HMC5883L. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct oled_display oled_display_t;

#define OLED_DISPLAY_WIDTH 128U
#define OLED_DISPLAY_HEIGHT 64U

typedef enum {
    OLED_BITMAP_BIT_ORDER_MSB_FIRST = 0,
    OLED_BITMAP_BIT_ORDER_LSB_FIRST,
} oled_bitmap_bit_order_t;

typedef enum {
    OLED_BITMAP_MODE_COPY = 0,
    OLED_BITMAP_MODE_TRANSPARENT,
} oled_bitmap_mode_t;

typedef struct {
    const uint8_t *data;
    size_t stride_bytes;
    uint16_t width;
    uint16_t height;
    oled_bitmap_bit_order_t bit_order;
    oled_bitmap_mode_t mode;
} oled_bitmap_t;

oled_display_t *oled_display_create(void);
void oled_display_destroy(oled_display_t *display);
esp_err_t oled_display_clear(oled_display_t *display);
esp_err_t oled_display_draw_text(oled_display_t *display,
                                 int x, int y, const char *text,
                                 uint8_t scale, bool on);
esp_err_t oled_display_draw_pixel(oled_display_t *display,
                                  int x, int y, bool on);
esp_err_t oled_display_draw_bitmap(oled_display_t *display,
                                   int x, int y,
                                   const oled_bitmap_t *bitmap,
                                   bool foreground_on);
esp_err_t oled_display_flush(oled_display_t *display);
esp_err_t oled_display_show_status(oled_display_t *display,
                                   uint8_t status, uint8_t frame_count);
/* Briefly exposes the local setup credentials during startup. */
esp_err_t oled_display_show_wifi_setup(oled_display_t *display,
                                       const char *ssid,
                                       const char *password);
