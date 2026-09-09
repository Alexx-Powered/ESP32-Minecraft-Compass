/*
 * SSD1306 OLED support using the ESP-IDF esp_lcd component.
 *
 * The SSD1306 is a monochrome, page-oriented controller. The framebuffer
 * below is stored as 128 columns per page, with each byte holding eight
 * vertical pixels (bit 0 is the top pixel of the page). This is the format
 * expected by esp_lcd_panel_draw_bitmap() for a full-screen flush.
 */

#include "oled_display.h"
#include "oled_font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "i2cdev.h"

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_SDA_GPIO GPIO_NUM_1
#define OLED_I2C_SCL_GPIO GPIO_NUM_2
#define OLED_I2C_ADDRESS 0x3CU
#define OLED_I2C_CLOCK_HZ (400 * 1000)
#define OLED_I2C_PROBE_TIMEOUT_MS 100
#define OLED_WIDTH OLED_DISPLAY_WIDTH
#define OLED_HEIGHT OLED_DISPLAY_HEIGHT
#define OLED_FRAMEBUFFER_SIZE (OLED_WIDTH * OLED_HEIGHT / 8U)
#define OLED_TEXT_COLUMN_SPACING 1
#define OLED_TEXT_LINE_SPACING 2
#define OLED_TEXT_TAB_SPACES 4

static const char *TAG = "oled";

struct oled_display {
    /* The bus handle is borrowed from i2cdev; only panel objects and the
     * framebuffer are owned by this adapter. */
    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    uint8_t framebuffer[OLED_FRAMEBUFFER_SIZE];
};

static void set_pixel(oled_display_t *display, int x, int y, bool on)
{
    /* Bounds checking here keeps text/bitmap helpers safe at the display edge. */
    if (display == NULL || (unsigned)x >= OLED_WIDTH || (unsigned)y >= OLED_HEIGHT) {
        return;
    }

    const size_t index = (size_t)(y >> 3) * OLED_WIDTH + (size_t)x;
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) {
        display->framebuffer[index] |= mask;
    } else {
        display->framebuffer[index] &= (uint8_t)~mask;
    }
}

static esp_err_t validate_text(const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            continue;
        }
        if (*p < OLED_FONT_FIRST || *p > OLED_FONT_LAST) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    return ESP_OK;
}

static void draw_text_unlocked(oled_display_t *display, int x, int y,
                               const char *text, uint8_t scale, bool on)
{
    int cursor_x = x;
    int cursor_y = y;
    const int glyph_width = (int)OLED_FONT_GLYPH_WIDTH;
    const int glyph_height = (int)OLED_FONT_GLYPH_HEIGHT;
    const int character_advance =
        (glyph_width + OLED_TEXT_COLUMN_SPACING) * (int)scale;

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p == '\r') {
            continue;
        }
        if (*p == '\n') {
            cursor_x = x;
            cursor_y += glyph_height * (int)scale + OLED_TEXT_LINE_SPACING;
            continue;
        }
        if (*p == '\t') {
            cursor_x += character_advance * OLED_TEXT_TAB_SPACES;
            continue;
        }

        const uint8_t *glyph = oled_font_5x7_glyph(*p);
        for (int column = 0; column < glyph_width; ++column) {
            for (int row = 0; row < glyph_height; ++row) {
                if ((glyph[column] & (uint8_t)(1U << row)) == 0U) {
                    continue;
                }
                for (int sx = 0; sx < scale; ++sx) {
                    for (int sy = 0; sy < scale; ++sy) {
                        set_pixel(display,
                                  cursor_x + column * (int)scale + sx,
                                  cursor_y + row * (int)scale + sy,
                                  on);
                    }
                }
            }
        }
        cursor_x += character_advance;
    }
}

static void cleanup_display(oled_display_t *display)
{
    if (display == NULL) {
        return;
    }

    if (display->panel != NULL) {
        (void)esp_lcd_panel_del(display->panel);
        display->panel = NULL;
    }
    if (display->panel_io != NULL) {
        (void)esp_lcd_panel_io_del(display->panel_io);
        display->panel_io = NULL;
    }
    /* i2cdev owns the shared bus for the HMC5883L and the OLED. */
    if (display->i2c_bus != NULL) {
        display->i2c_bus = NULL;
    }
    free(display);
}

oled_display_t *oled_display_create(void)
{
    /* Probe before constructing the panel so an absent OLED cleanly degrades to
     * WS2812 plus logs, while the shared I2C bus remains available to HMC5883L. */
    oled_display_t *display = calloc(1, sizeof(*display));
    if (display == NULL) {
        ESP_LOGW(TAG, "OLED allocation failed");
        return NULL;
    }

    esp_err_t err = i2cdev_get_shared_handle(
        OLED_I2C_PORT, (void **)&display->i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Shared I2C bus unavailable: %s; OLED disabled",
                 esp_err_to_name(err));
        cleanup_display(display);
        return NULL;
    }

    err = i2c_master_probe(display->i2c_bus, OLED_I2C_ADDRESS,
                           OLED_I2C_PROBE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not detected at 0x%02X: %s; OLED disabled",
                 OLED_I2C_ADDRESS, esp_err_to_name(err));
        cleanup_display(display);
        return NULL;
    }

    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_CLOCK_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .transaction_timeout_ms = OLED_I2C_PROBE_TIMEOUT_MS,
    };
    err = esp_lcd_new_panel_io_i2c(
        display->i2c_bus,
        &io_config,
        &display->panel_io
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 panel IO init failed: %s; OLED disabled",
                 esp_err_to_name(err));
        cleanup_display(display);
        return NULL;
    }

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = OLED_HEIGHT,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = GPIO_NUM_NC,
        .vendor_config = &ssd1306_config,
    };
    err = esp_lcd_new_panel_ssd1306(display->panel_io, &panel_config,
                                    &display->panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 panel creation failed: %s; OLED disabled",
                 esp_err_to_name(err));
        cleanup_display(display);
        return NULL;
    }

    err = esp_lcd_panel_reset(display->panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(display->panel);
    }
    if (err == ESP_OK) {
        // Preserve the orientation used by the previous driver (A1/C8).
        err = esp_lcd_panel_mirror(display->panel, true, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(display->panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 panel initialization failed: %s; OLED disabled",
                 esp_err_to_name(err));
        cleanup_display(display);
        return NULL;
    }

    ESP_LOGI(TAG, "SSD1306 ready on shared I2C%d: SDA=%d SCL=%d address=0x%02X",
             OLED_I2C_PORT, OLED_I2C_SDA_GPIO, OLED_I2C_SCL_GPIO,
             OLED_I2C_ADDRESS);
    return display;
}

void oled_display_destroy(oled_display_t *display)
{
    cleanup_display(display);
}

esp_err_t oled_display_clear(oled_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display->framebuffer, 0, sizeof(display->framebuffer));
    return ESP_OK;
}

esp_err_t oled_display_draw_pixel(oled_display_t *display,
                                  int x,
                                  int y,
                                  bool on)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    set_pixel(display, x, y, on);
    return ESP_OK;
}

esp_err_t oled_display_draw_text(oled_display_t *display,
                                 int x,
                                 int y,
                                 const char *text,
                                 uint8_t scale,
                                 bool on)
{
    if (display == NULL || text == NULL || scale == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_text(text);
    if (err != ESP_OK) {
        return err;
    }

    draw_text_unlocked(display, x, y, text, scale, on);
    return ESP_OK;
}

esp_err_t oled_display_draw_bitmap(oled_display_t *display,
                                   int x,
                                   int y,
                                   const oled_bitmap_t *bitmap,
                                   bool foreground_on)
{
    if (display == NULL || bitmap == NULL || bitmap->data == NULL ||
        bitmap->width == 0U || bitmap->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bitmap->bit_order != OLED_BITMAP_BIT_ORDER_MSB_FIRST &&
        bitmap->bit_order != OLED_BITMAP_BIT_ORDER_LSB_FIRST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bitmap->mode != OLED_BITMAP_MODE_COPY &&
        bitmap->mode != OLED_BITMAP_MODE_TRANSPARENT) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t minimum_stride = ((size_t)bitmap->width + 7U) / 8U;
    const size_t stride = bitmap->stride_bytes == 0U
                              ? minimum_stride
                              : bitmap->stride_bytes;
    if (stride < minimum_stride) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t row = 0; row < bitmap->height; ++row) {
        const uint8_t *source_row = bitmap->data + (size_t)row * stride;
        for (uint16_t column = 0; column < bitmap->width; ++column) {
            const uint8_t source_byte = source_row[column >> 3];
            const uint8_t source_mask = bitmap->bit_order ==
                                                OLED_BITMAP_BIT_ORDER_MSB_FIRST
                                            ? (uint8_t)(0x80U >> (column & 7U))
                                            : (uint8_t)(1U << (column & 7U));
            const bool bit_set = (source_byte & source_mask) != 0U;

            if (bitmap->mode == OLED_BITMAP_MODE_TRANSPARENT && !bit_set) {
                continue;
            }

            set_pixel(display,
                      x + (int)column,
                      y + (int)row,
                      bit_set ? foreground_on : !foreground_on);
        }
    }

    return ESP_OK;
}

esp_err_t oled_display_flush(oled_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display->panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_lcd_panel_draw_bitmap(display->panel,
                                     0, 0, OLED_WIDTH, OLED_HEIGHT,
                                     display->framebuffer);
}

esp_err_t oled_display_show_status(oled_display_t *display,
                                   uint8_t status,
                                   uint8_t frame_count)
{
    if (display == NULL || frame_count == 0U || status == 0U ||
        status > frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    char status_line[20];
    char frame_line[20];
    (void)snprintf(status_line, sizeof(status_line), "STATUS: %02u",
                   (unsigned)status);
    (void)snprintf(frame_line, sizeof(frame_line), "FRAME %02u/%02u",
                   (unsigned)status, (unsigned)frame_count);

    esp_err_t err = oled_display_clear(display);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_display_draw_text(display, 2, 2, "MODE: STATUS0", 1, true);
    if (err != ESP_OK) {
        return err;
    }
    err = oled_display_draw_text(display, 2, 18, status_line, 2, true);
    if (err != ESP_OK) {
        return err;
    }
    err = oled_display_draw_text(display, 2, 48, frame_line, 1, true);
    if (err != ESP_OK) {
        return err;
    }

    return oled_display_flush(display);
}

esp_err_t oled_display_show_wifi_setup(oled_display_t *display,
                                       const char *ssid,
                                       const char *password)
{
    if (display == NULL || ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char ssid_line[24];
    char password_line[24];
    (void)snprintf(ssid_line, sizeof(ssid_line), "SSID:%s", ssid);
    (void)snprintf(password_line, sizeof(password_line), "PASS:%s", password);

    esp_err_t err = oled_display_clear(display);
    if (err == ESP_OK) {
        err = oled_display_draw_text(display, 2, 0, "WIFI CONFIG", 1, true);
    }
    if (err == ESP_OK) {
        err = oled_display_draw_text(display, 2, 11, ssid_line, 1, true);
    }
    if (err == ESP_OK) {
        err = oled_display_draw_text(display, 2, 22, password_line, 1, true);
    }
    if (err == ESP_OK) {
        err = oled_display_draw_text(display, 2, 36,
                                     "OPEN 192.168.4.1", 1, true);
    }
    if (err == ESP_OK) {
        err = oled_display_draw_text(display, 2, 50,
                                     "SAVED IN NVS", 1, true);
    }
    return err == ESP_OK ? oled_display_flush(display) : err;
}
